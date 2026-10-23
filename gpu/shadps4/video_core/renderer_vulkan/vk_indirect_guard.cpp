// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_indirect_guard.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/indirect_guard_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
struct Push {
    u32 max_count[3];
    u32 first;
    u32 max_total;
    u32 slot;
    u32 draw;
};

struct Skipped {
    u32 count;
    u32 draws;
    u32 pad[2];
    u32 last[3];
    u32 pad2;
    u32 draw_last[3];
    u32 largest;      ///< groups of the largest dispatch that ran
    u32 draw_largest; ///< vertices (indices) x instances of the largest draw that ran
};

s64 NowNs() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}
} // namespace

bool IndirectGuard::Enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("BB_INDIRECT_GUARD");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

IndirectGuard::IndirectGuard(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const auto device = instance.GetDevice();
    checked = std::make_unique<VideoCore::Buffer>(instance, 0, NumSlots * SlotSize,
                                                  VideoCore::MemoryType::DeviceLocal,
                                                  "Indirect arguments checked");
    skipped = std::make_unique<VideoCore::Buffer>(instance, 0, sizeof(Skipped),
                                                  VideoCore::MemoryType::HostCached,
                                                  "Indirect dispatches skipped");
    std::memset(skipped->mapped_data.data(), 0, sizeof(Skipped));
    skipped->Flush(0, sizeof(Skipped));

    const auto limits = instance.GetPhysicalDevice().getProperties().limits;
    for (u32 i = 0; i < 3; ++i) {
        max_count[i] = limits.maxComputeWorkGroupCount[i];
    }
    const char* max = std::getenv("BB_INDIRECT_GUARD_MAX");
    max_total = max && *max ? static_cast<u32>(std::strtoul(max, nullptr, 0)) : 1u << 16;
    const char* draw_max = std::getenv("BB_INDIRECT_DRAW_MAX");
    max_draw_total =
        draw_max && *draw_max ? static_cast<u32>(std::strtoul(draw_max, nullptr, 0)) : 1u << 20;

    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = vk::DescriptorType::eStorageBuffer,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(Push)};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(INDIRECT_GUARD_COMP, device);
    pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
    last_report_ns = NowNs();
}

IndirectGuard::~IndirectGuard() = default;

std::pair<vk::Buffer, u64> IndirectGuard::CheckArgs(const VideoCore::Buffer& args, u64 offset) {
    return Guard(args, offset, 3, false);
}

std::pair<vk::Buffer, u64> IndirectGuard::CheckDrawArgs(const VideoCore::Buffer& args, u64 offset,
                                                        u32 words) {
    return Guard(args, offset, words, true);
}

std::pair<vk::Buffer, u64> IndirectGuard::Guard(const VideoCore::Buffer& args, u64 offset,
                                                u32 words, bool draw) {
    // A storage binding starts at a multiple of the device's alignment: the arguments are
    // `first` dwords into it.
    const u64 alignment = instance.StorageMinAlignment();
    const u64 bind_offset = offset / alignment * alignment;
    const u32 first = static_cast<u32>((offset - bind_offset) / sizeof(u32));
    const u64 bind_size = (offset - bind_offset) + words * sizeof(u32);
    const u32 slot = next_slot.fetch_add(1, std::memory_order_relaxed) % NumSlots;
    const Push push{{max_count[0], max_count[1], max_count[2]},
                    first,
                    draw ? max_draw_total : max_total,
                    slot * SlotSize / u32(sizeof(u32)),
                    draw ? words : 0};
    scheduler.Record([this, source = args.Handle(), bind_offset, bind_size,
                      push](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                             vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {source, bind_offset, bind_size},
            {checked->Handle(), 0, NumSlots * SlotSize},
            {skipped->Handle(), 0, sizeof(Skipped)},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
        cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             &push);
        cmdbuf.dispatch(1, 1, 1);
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect |
                            vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead |
                             vk::AccessFlagBits2::eShaderStorageRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    });
    Report();
    return {checked->Handle(), u64(slot) * SlotSize};
}

void IndirectGuard::Report() {
    const s64 now = NowNs();
    s64 last = last_report_ns.load(std::memory_order_relaxed);
    if (now - last < 5'000'000'000 ||
        !last_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return;
    }
    skipped->Invalidate(0, sizeof(Skipped));
    const auto* s = reinterpret_cast<const volatile Skipped*>(skipped->mapped_data.data());
    const u32 count = s->count;
    const u32 before = reported.exchange(count, std::memory_order_relaxed);
    if (count != before) {
        std::printf("Indirect dispatches: %u skipped in all (garbage group counts, the last "
                    "%ux%ux%u; BB_INDIRECT_GUARD)\n",
                    count, s->last[0], s->last[1], s->last[2]);
    }
    const u32 draws = s->draws;
    const u32 draws_before = reported_draws.exchange(draws, std::memory_order_relaxed);
    if (draws != draws_before) {
        std::printf("Indirect draws: %u skipped in all (garbage counts, the last %u vertices x %u "
                    "instances from %u; BB_INDIRECT_DRAW_MAX)\n",
                    draws, s->draw_last[0], s->draw_last[1], s->draw_last[2]);
    }
    // The largest counts that ran: the game's real ones, to set the limits by.
    const u32 largest = s->largest, draw_largest = s->draw_largest;
    if (largest != reported_largest.exchange(largest, std::memory_order_relaxed) ||
        draw_largest != reported_draw_largest.exchange(draw_largest, std::memory_order_relaxed)) {
        std::printf("Indirect arguments: the largest that ran: dispatch %u groups (limit %u), draw "
                    "%u vertices x instances (limit %u)\n",
                    largest, max_total, draw_largest, max_draw_total);
    }
}

} // namespace Vulkan
