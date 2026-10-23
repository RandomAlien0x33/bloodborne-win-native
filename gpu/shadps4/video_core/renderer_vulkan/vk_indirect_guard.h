// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: indirect dispatches run from checked group counts.
//
// The game's indirect dispatches take their group counts from memory a shader wrote. Twice
// (issues #34 and #32, the PC memory model) those counts were garbage — 1.2 and 3.8 billion
// groups of the particle simulations — and the GPU, still running them after the kernel's
// timeout, was reset: device lost, the game gone. Bloodborne's real counts are at most a few
// hundred. Before each indirect dispatch a one-thread shader (indirect_guard.comp) copies the
// counts to a slot of our own, zero if they are implausible (past the device's limits or
// BB_INDIRECT_GUARD_MAX groups in all, default 64K), and the dispatch reads that slot. Skipped
// dispatches are counted and reported. BB_INDIRECT_GUARD=0: the game's counts as they are.
//
// Indirect draws the same way: in a Chalice Dungeon boss fight one indirect draw (a vertex shader
// without loops, at most one draw) kept the GPU busy for good without a reset, the game frozen:
// a count no real draw has. A vertex or index count times instances past BB_INDIRECT_DRAW_MAX
// (default 1M) draws nothing, and is counted and reported too. Garbage below a looser limit
// (16M: half a million particles at random places) still ran at 1 FPS for seconds; the largest
// counts that ran are reported to set the limits by.
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {
struct Buffer;
}

namespace Vulkan {

class Instance;
class Scheduler;

class IndirectGuard {
public:
    IndirectGuard(const Instance& instance, Scheduler& scheduler);
    ~IndirectGuard();

    static bool Enabled();

    /// Records the check of the arguments at `offset` in `args` (outside a render pass); returns
    /// the buffer and offset to dispatch from.
    std::pair<vk::Buffer, u64> CheckArgs(const VideoCore::Buffer& args, u64 offset);

    /// The same for one indirect draw's arguments (`words` dwords: 4, or 5 indexed).
    std::pair<vk::Buffer, u64> CheckDrawArgs(const VideoCore::Buffer& args, u64 offset, u32 words);

private:
    static constexpr u32 NumSlots = 1024;
    static constexpr u32 SlotSize = 32; ///< bytes: a dispatch's 3 dwords or a draw's 4-5

    std::pair<vk::Buffer, u64> Guard(const VideoCore::Buffer& args, u64 offset, u32 words,
                                     bool draw);
    void Report();

    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VideoCore::Buffer> checked; ///< NumSlots x SlotSize bytes of arguments
    std::unique_ptr<VideoCore::Buffer> skipped; ///< count and the last skipped counts (host)
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
    u32 max_count[3]{};
    u32 max_total = 0;
    u32 max_draw_total = 0;
    std::atomic<u32> next_slot{0};
    std::atomic<u32> reported{0};
    std::atomic<u32> reported_draws{0};
    std::atomic<u32> reported_largest{0};
    std::atomic<u32> reported_draw_largest{0};
    std::atomic<s64> last_report_ns{0};
};

} // namespace Vulkan
