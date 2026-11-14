// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "common/serdes.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/copy_shader.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_list.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 10u; // motion buffers' addresses from a binding
static constexpr u32 ShaderMetaVersion = 8u; // bbport: vertex shaders' MotionAddresses binding
static constexpr u32 PipelineKeyVersion = 5u; // bbport: Info layout (ImageResource::needs_native)
} // namespace Serialization

namespace Vulkan {

/// Shader replay test: the last replayed stage's flattened user data had the size it expects.
thread_local bool replay_flat_ok = true;

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    if (!info.Deserialize(ar)) {
        return false;
    }
    // Motion vertex shaders load too: they read the motion buffers' addresses from a binding
    // (MotionAddresses), and while object motion is off no draw selects them.

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

struct PipelineCache::PreloadJob {
    bool compute = false;
    ComputePipelineKey compute_key{};
    GraphicsPipelineKey graphics_key{};
    ComputePipeline::SerializationSupport compute_data{};
    GraphicsPipeline::SerializationSupport graphics_data{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch{};
    std::unique_ptr<ComputePipeline> compute_pipeline;
    std::unique_ptr<GraphicsPipeline> graphics_pipeline;
    std::string error; ///< the driver rejected it (a damaged entry)
};

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar, PreloadJob* defer) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }
    if (defer) {
        defer->compute = true;
        defer->compute_key = compute_key;
        defer->compute_data = sdata;
        defer->infos = sel.infos;
        defer->modules = sel.modules;
        sel.infos.fill(nullptr);
        sel.modules.fill(nullptr);
        return true;
    }

    // bbport: built before it is inserted: a pipeline the driver rejects while preloading throws
    // (Serialization::CorruptData) and leaves no empty entry behind.
    auto pipeline =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *sel.infos[0], sel.modules[0], sdata, true);
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);
    it.value() = std::move(pipeline);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar, PreloadJob* defer) {
    sel.graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }
    if (defer) {
        defer->graphics_key = sel.graphics_key;
        defer->graphics_data = std::move(sdata);
        defer->runtime_infos = sel.runtime_infos;
        defer->infos = sel.infos;
        defer->modules = sel.modules;
        defer->fetch = std::move(sel.fetch_shader);
        sel.infos.fill(nullptr);
        sel.modules.fill(nullptr);
        sel.fetch_shader.reset();
        return true;
    }

    auto pipeline = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
        sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, true);
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    ASSERT(is_new);
    it.value() = std::move(pipeline);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, sel.fetch_shader, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    // bbport: a SPIR-V binary starts with its magic number and a 5-word header; anything else is
    // a damaged file (crash or power loss while it was written) the driver must not see.
    if (spv.size() < 5 || spv[0] != 0x07230203u) {
        if (!spv.empty()) {
            throw Serialization::CorruptData{"damaged SPIR-V in the shader cache"};
        }
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};
    // bbport: a module the driver rejects is a damaged entry (WarmUp rebuilds the cache), not a
    // fatal error as in CompileSPV.
    const auto compile = [&] {
        auto [result, created] = instance.GetDevice().createShaderModule(
            {.codeSize = spv.size() * sizeof(u32), .pCode = spv.data()});
        if (result != vk::Result::eSuccess) {
            throw Serialization::CorruptData{"cached SPIR-V rejected by the driver"};
        }
        return created;
    };

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = compile();
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = compile();
        }
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    sel.infos[stage] = &it_pgm.value()->info;
    sel.modules[stage] = module;

    return true;
}

namespace {
constexpr std::array<u32, 4> CacheVersions{0x42425043u, Serialization::ShaderBinaryVersion,
                                           Serialization::ShaderMetaVersion,
                                           Serialization::PipelineKeyVersion};
} // namespace

void PipelineCache::SaveCacheProfile() {
    constexpr size_t header_size = sizeof(CacheVersions);
    std::vector<u8> current(header_size + sizeof(profile));
    std::memcpy(current.data(), CacheVersions.data(), header_size);
    std::memcpy(current.data() + header_size, &profile, sizeof(profile));
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                       std::move(current));
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    Storage::DataBase::Instance().Open();

    // Shader metadata and SPIR-V filenames share permutation indices. After a backend version
    // change, retaining old blobs alongside new ones can associate a valid metadata entry with
    // a different binary at the same index. Migrate the entire cache before loading any modules.
    const auto& cache_versions = CacheVersions;
    constexpr size_t header_size = sizeof(CacheVersions);
    const auto save_profile = [this] { SaveCacheProfile(); };
    // Check both compiler/cache versions and the device profile.
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        // A missing profile also means any remaining blobs have unknown compatibility.
        Storage::DataBase::Instance().Clear();
        save_profile();
    } else {
        const bool versions_match =
            profile_data.size() == header_size + sizeof(Shader::Profile) &&
            std::memcmp(profile_data.data(), cache_versions.data(), header_size) == 0;
        Shader::Profile cached_profile{};
        if (versions_match) {
            std::memcpy(&cached_profile, profile_data.data() + header_size,
                        sizeof(cached_profile));
        }
        if (!versions_match || cached_profile != profile) {
            // bbport: upstream closed the cache for the session here, so it was never rewritten
            // and every later session compiled every shader again (stutters on each new area).
            // Start a fresh cache for this build and GPU instead.
            LOG_WARNING(Render, "Pipeline cache isn't compatible with current compiler/system: rebuilding it");
            Storage::DataBase::Instance().Clear();
            save_profile();
        }
    }

    // bbport: the shipped list's import and the pipelines' creation (slow at a first start)
    // wait for Prepare, called once the presenter can show their progress.
}

void PipelineCache::Prepare(const ProgressFunc& progress) {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        FinishDriverCacheLoad(false);
        return;
    }
    const auto save_profile = [this] { SaveCacheProfile(); };
    // The shipped list's shaders join the cache before it is loaded (a fresh cache too: then
    // they are all it holds).
    const u64 list_import = ImportPipelineList(progress);

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    u32 num_damaged{};

    std::vector<std::vector<u8>> keys;
    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey,
        [&](std::vector<u8>&& data) { keys.push_back(std::move(data)); });
    // The stages are loaded here one by one (shared program state); the driver's part, creating
    // each pipeline, is the slow one and runs below on every hardware thread.
    std::vector<std::unique_ptr<PreloadJob>> jobs;
    jobs.reserve(keys.size());
    for (auto& data : keys) {
        ++num_total_pipelines;
        // bbport: a damaged entry (cut short by a crash or a power loss, or rejected by the
        // driver) used to stop the game at every start until the cache was deleted by hand
        // (issues #28, #38, #39). It is counted here and the cache is rebuilt below.
        try {
            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                continue;
            }

            u32 is_compute{};
            pldata.Read(is_compute);

            auto job = std::make_unique<PreloadJob>();
            const bool result = is_compute ? LoadComputePipeline(ar, job.get())
                                           : LoadGraphicsPipeline(ar, job.get());
            if (result) {
                jobs.push_back(std::move(job));
            }
        } catch (const std::exception& e) {
            if (num_damaged++ == 0) {
                LOG_WARNING(Render, "Pipeline cache: damaged entry ({})", e.what());
            }
            sel.infos.fill(nullptr);
            sel.modules.fill(nullptr);
            sel.fetch_shader.reset();
        }
    }

    // The driver creates pipelines on any number of threads at once (VkPipelineCache is
    // internally synchronized); one per hardware thread of this machine.
    const u32 hardware = std::max(std::thread::hardware_concurrency(), 1u);
    u32 threads = hardware;
    if (const char* env = std::getenv("BB_PREPARE_THREADS"); env && *env) {
        threads = std::max(u32(std::strtoul(env, nullptr, 10)), 1u);
    }
    threads = u32(std::min<size_t>(threads, std::max<size_t>(jobs.size(), 1)));
    const auto created_from = std::chrono::steady_clock::now();
    std::atomic<size_t> next{0}, finished{0};
    const auto create = [&] {
        for (size_t i; (i = next.fetch_add(1)) < jobs.size(); finished.fetch_add(1)) {
            auto& job = *jobs[i];
            try {
                if (job.compute) {
                    job.compute_pipeline = std::make_unique<ComputePipeline>(
                        instance, scheduler, desc_heap, profile, *pipeline_cache, job.compute_key,
                        *job.infos[0], job.modules[0], job.compute_data, true);
                } else {
                    job.graphics_pipeline = std::make_unique<GraphicsPipeline>(
                        instance, scheduler, desc_heap, profile, job.graphics_key,
                        *pipeline_cache, job.infos, job.runtime_infos, job.fetch, job.modules,
                        job.graphics_data, true);
                }
            } catch (const std::exception& e) {
                job.error = e.what();
            }
        }
    };
    std::vector<std::thread> workers;
    for (u32 i = 0; i + 1 < threads; ++i) {
        workers.emplace_back([&] {
            Common::SetCurrentThreadName("bb:PipePrep");
            create();
        });
    }
    // This thread shows the progress (one worker fewer stands for it).
    const u32 skipped = num_total_pipelines - u32(jobs.size());
    if (workers.empty()) {
        create();
    } else {
        while (finished.load() < jobs.size()) {
            progress(1, skipped + u32(finished.load()), num_total_pipelines);
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (auto& job_ptr : jobs) {
        auto& job = *job_ptr;
        if (!job.error.empty()) {
            if (num_damaged++ == 0) {
                LOG_WARNING(Render, "Pipeline cache: damaged entry ({})", job.error);
            }
            continue;
        }
        if (job.compute) {
            if (auto [it, is_new] = compute_pipelines.try_emplace(job.compute_key); is_new) {
                it.value() = std::move(job.compute_pipeline);
                ++num_pipelines;
            }
        } else {
            if (auto [it, is_new] = graphics_pipelines.try_emplace(job.graphics_key); is_new) {
                it.value() = std::move(job.graphics_pipeline);
                ++num_pipelines;
            }
        }
    }
    jobs.clear();
    progress(1, num_total_pipelines, num_total_pipelines);
    std::printf("GPU: %u cached pipelines created in %.1f s on %u threads (%u hardware threads)\n",
                num_pipelines,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - created_from)
                    .count(),
                threads, hardware);

    if (num_damaged) {
        // Nothing preloaded is trusted: modules of a damaged entry may sit in programs that later
        // lookups would reuse. Start as with no cache and write a fresh one.
        LOG_WARNING(Render, "Pipeline cache: {} damaged entries, rebuilding it", num_damaged);
        graphics_pipelines.clear();
        compute_pipelines.clear();
        std::unordered_set<VkShaderModule> modules;
        for (const auto& [_, program] : program_cache) {
            if (!program) {
                continue;
            }
            for (const auto& permutation : program->modules) {
                if (permutation.module) {
                    modules.insert(VkShaderModule(permutation.module));
                }
            }
        }
        for (const VkShaderModule module : modules) {
            instance.GetDevice().destroyShaderModule(vk::ShaderModule{module});
        }
        program_cache.clear();
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        save_profile();
        if (list_import) {
            // A list whose entries the driver rejects is not imported again.
            SavePipelineListState(list_import, false);
        }
        FinishDriverCacheLoad(true);
        return;
    }
    if (list_import) {
        SavePipelineListState(list_import, false);
    }

    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    if (const char* code_dir = std::getenv("BB_SHADER_REPLAY_TEST")) {
        ReplayTest(code_dir);
    }
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
    FinishDriverCacheLoad(false);
}

std::vector<u32> PipelineCache::CompileStoredStage(
    const Shader::Info& stored, const Shader::StageSpecialization& spec,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch, size_t perm_idx,
    std::span<const u32> code, std::span<const u32> copy_code) {
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    std::copy_n(stored.flattened_ud_buf.begin(),
                std::min(user_data.size(), stored.flattened_ud_buf.size()), user_data.begin());
    const Shader::ShaderParams params{.user_data = user_data, .code = code, .hash = stored.pgm_hash};
    Shader::Info info{stored.hw_stage, stored.sw_stage, params};
    Shader::ReplayInputs inputs{.flat = stored.flattened_ud_buf, .fetch = fetch};
    if (fetch) {
        for (size_t i = 0; i < fetch->attributes.size() && i < spec.vs_attribs.size(); ++i) {
            inputs.attribs.push_back({fetch->attributes[i].semantic, spec.vs_attribs[i].num_class,
                                      spec.vs_attribs[i].dst_select});
        }
    }
    auto runtime_info = spec.runtime_info;
    // A first permutation's specialization is taken after its compile, which added the emulated
    // clip distances' input to the fragment inputs.
    if (perm_idx == 0 && stored.sw_stage == Shader::SwStage::Fragment &&
        runtime_info.hw.fs.clip_distance_emulation) {
        --runtime_info.hw.fs.num_inputs;
    }
    if (stored.hw_stage == Shader::HwStage::Geometry) {
        runtime_info.hw.gs.vs_copy = copy_code;
        if (copy_code.empty() || Shader::ParseCopyShader(copy_code).output_vertices == 0) {
            return {};
        }
    }
    // Vertex motion is compiled in only once object motion has found the game's buffers; a
    // stage stored with it has it.
    const u64 motion_positions = Shader::MotionVectors::positions_address;
    if (motion_positions == 0) {
        Shader::MotionVectors::positions_address = 1;
    }
    auto binding = spec.start;
    Shader::replay_inputs = &inputs;
    replay_flat_ok = true;
    std::vector<u32> spv;
    try {
        const auto program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
        spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
    } catch (const std::exception& e) {
        LOG_WARNING(Render, "Pipeline list: {} {:#x}_{} failed to compile: {}", stored.hw_stage,
                    stored.pgm_hash, perm_idx, e.what());
        spv.clear();
    }
    Shader::replay_inputs = nullptr;
    Shader::MotionVectors::positions_address = motion_positions;
    if (!replay_flat_ok) {
        // Its walker of resource tables fills another size than the stored data: the stored
        // description is from another compiler.
        spv.clear();
    }
    return spv;
}

void PipelineCache::SavePipelineListState(u64 id, bool preparing) {
    std::vector<u8> state(sizeof(id) + 1);
    std::memcpy(state.data(), &id, sizeof(id));
    state[sizeof(id)] = preparing ? 1 : 0;
    auto& db = Storage::DataBase::Instance();
    db.Save(Storage::BlobType::ShaderProfile, "pipeline_list", std::move(state));
    db.WaitWrites();
}

u64 PipelineCache::ImportPipelineList(const ProgressFunc& progress) {
    auto& db = Storage::DataBase::Instance();
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        return 0;
    }
    const auto path = PipelineList::Path();
    std::vector<PipelineList::Entry> entries;
    u64 id{};
    if (!PipelineList::Read(path, entries, id)) {
        return 0;
    }
    // The list's state in this cache: done, or still being prepared (compiled, then loaded by
    // the driver). A start that ended in between (a crash in a list's shader or pipeline) must
    // not end every later start the same way: the cache is rebuilt without the list.
    std::vector<u8> state;
    if (db.Exists(Storage::BlobType::ShaderProfile, "pipeline_list")) {
        db.Load(Storage::BlobType::ShaderProfile, "pipeline_list", state);
    }
    if (state.size() == sizeof(id) + 1 && std::memcmp(state.data(), &id, sizeof(id)) == 0) {
        if (state[sizeof(id)] == 0) {
            return 0;
        }
        LOG_WARNING(Render, "Pipeline list: the last start stopped while its shaders were "
                            "prepared; the cache is rebuilt without it");
        db.Clear();
        SaveCacheProfile();
        SavePipelineListState(id, false);
        return 0;
    }
    SavePipelineListState(id, true);
    const auto started = std::chrono::steady_clock::now();

    struct Stage {
        const PipelineList::Entry* entry;
        Shader::Info info;
        Shader::StageSpecialization spec;
        std::optional<Shader::Gcn::FetchShaderData> fetch;
        size_t perm_idx;
        u64 perm_hash;
    };
    std::vector<std::unique_ptr<Stage>> stages;
    std::unordered_set<u64> wanted;
    std::unordered_map<u64, bool> program_in_cache;
    u32 other_build{}, in_cache{};
    for (const auto& entry : entries) {
        if (entry.type != u8(Storage::BlobType::ShaderMeta)) {
            continue;
        }
        auto stage = std::make_unique<Stage>();
        stage->entry = &entry;
        stage->spec.info = &stage->info;
        stage->perm_hash = std::strtoull(entry.name.c_str(), nullptr, 16);
        try {
            Serialization::Archive ar{std::vector<u8>{entry.data}};
            if (!LoadShaderMeta(ar, stage->info, stage->fetch, stage->spec, stage->perm_idx)) {
                ++other_build;
                continue;
            }
        } catch (const std::exception&) {
            ++other_build;
            continue;
        }
        // A program the game already compiled here keeps its own permutations: the list's
        // indices for it could name other ones.
        const u64 hash = stage->info.pgm_hash;
        auto [it, is_new] = program_in_cache.try_emplace(hash, false);
        if (is_new) {
            it->second = db.Exists(Storage::BlobType::ShaderBinary, fmt::format("{:#018x}_0", hash));
        }
        if (it->second) {
            ++in_cache;
            continue;
        }
        wanted.insert(hash);
        if (stage->info.hw_stage == Shader::HwStage::Geometry) {
            wanted.insert(stage->spec.runtime_info.hw.gs.vs_copy_hash);
        }
        stages.push_back(std::move(stage));
    }
    if (other_build) {
        LOG_WARNING(Render, "Pipeline list: {} shaders were described by another build, skipped",
                    other_build);
    }

    std::unordered_set<u64> compiled;
    u32 failed{}, no_code{};
    if (!stages.empty()) {
        LOG_WARNING(Render, "Pipeline list: compiling {} shaders for this GPU (once)",
                    stages.size());
        progress(0, 0, u32(stages.size()));
        const auto game = PipelineList::ReadGameShaders(wanted);
        u32 done{};
        for (const auto& stage : stages) {
            progress(0, done++, u32(stages.size()));
            const bool geometry = stage->info.hw_stage == Shader::HwStage::Geometry;
            const auto program = game.programs.find(stage->info.pgm_hash);
            const auto copy = geometry
                                  ? game.copies.find(stage->spec.runtime_info.hw.gs.vs_copy_hash)
                                  : game.copies.end();
            if (program == game.programs.end() || (geometry && copy == game.copies.end())) {
                ++no_code;
                continue;
            }
            auto spv = CompileStoredStage(stage->info, stage->spec, stage->fetch, stage->perm_idx,
                                          program->second,
                                          geometry ? std::span<const u32>{copy->second}
                                                   : std::span<const u32>{});
            if (spv.empty()) {
                ++failed;
                continue;
            }
            db.Save(Storage::BlobType::ShaderMeta, stage->entry->name,
                    std::vector<u8>{stage->entry->data});
            db.Save(Storage::BlobType::ShaderBinary,
                    fmt::format("{:#018x}_{}", stage->info.pgm_hash, stage->perm_idx),
                    std::move(spv));
            compiled.insert(stage->perm_hash);
        }
    }

    // A pipeline joins when every stage it names was compiled from the list just now.
    u32 pipelines{};
    for (const auto& entry : entries) {
        if (entry.type != u8(Storage::BlobType::PipelineKey)) {
            continue;
        }
        try {
            Serialization::Archive ar{std::vector<u8>{entry.data}};
            Serialization::Reader reader{ar};
            u32 version{}, is_compute{};
            reader.Read(version);
            reader.Read(is_compute);
            if (version != Serialization::PipelineKeyVersion) {
                continue;
            }
            bool complete = true;
            if (is_compute) {
                ComputePipelineKey key{};
                key.Deserialize(ar);
                complete = compiled.contains(key.value);
            } else {
                GraphicsPipelineKey key{};
                key.Deserialize(ar);
                for (const u64 hash : key.stage_hashes) {
                    complete &= hash == 0 || compiled.contains(hash);
                }
            }
            if (complete) {
                db.Save(Storage::BlobType::PipelineKey, entry.name, std::vector<u8>{entry.data});
                ++pipelines;
            }
        } catch (const std::exception&) {
        }
    }
    db.WaitWrites();
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    LOG_WARNING(Render,
                "Pipeline list: {} shaders compiled, {} pipelines added in {:.1f} s ({} already "
                "compiled here, {} without code in the game's files, {} failed)",
                compiled.size(), pipelines, seconds, in_cache, no_code, failed);
    return id;
}

void PipelineCache::ReplayTest(const char* code_dir) {
    u32 total{}, same{}, differ{}, no_code{}, no_spv{}, unreadable{};
    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::ShaderMeta, [&](std::vector<u8>&& data) {
            ++total;
            Shader::Info stored{};
            Shader::StageSpecialization spec{};
            spec.info = &stored;
            std::optional<Shader::Gcn::FetchShaderData> fetch;
            size_t perm_idx{};
            try {
                Serialization::Archive ar{std::move(data)};
                if (!LoadShaderMeta(ar, stored, fetch, spec, perm_idx)) {
                    ++unreadable;
                    return;
                }
            } catch (const std::exception&) {
                ++unreadable;
                return;
            }
            const auto read_code = [&](u64 hash, const char* kind) {
                std::vector<u32> words;
                std::ifstream file{fmt::format("{}/{:016x}{}.bin", code_dir, hash, kind),
                                   std::ios::binary};
                if (file) {
                    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
                    words.resize(bytes.size() / sizeof(u32));
                    std::memcpy(words.data(), bytes.data(), words.size() * sizeof(u32));
                }
                return words;
            };
            const std::vector<u32> code = read_code(stored.pgm_hash, "");
            std::vector<u32> copy_code;
            if (stored.hw_stage == Shader::HwStage::Geometry) {
                copy_code = read_code(spec.runtime_info.hw.gs.vs_copy_hash, ".copy");
            }
            if (code.empty()) {
                ++no_code;
                return;
            }
            std::vector<u32> cached;
            Storage::DataBase::Instance().Load(
                Storage::BlobType::ShaderBinary,
                fmt::format("{:#018x}_{}", stored.pgm_hash, perm_idx), cached);
            if (cached.empty()) {
                ++no_spv;
                return;
            }
            const auto spv = CompileStoredStage(stored, spec, fetch, perm_idx, code, copy_code);
            if (spv == cached) {
                ++same;
                return;
            }
            ++differ;
            LOG_WARNING(Render, "Shader replay: {} {:#x}_{} differs: {} words, cached {}",
                        stored.hw_stage, stored.pgm_hash, perm_idx, spv.size(), cached.size());
        });
    LOG_WARNING(Render,
                "Shader replay: {} stages: {} same, {} differ, {} without code, {} without "
                "cached SPIR-V, {} unreadable",
                total, same, differ, no_code, no_spv, unreadable);
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

thread_local const ReplayInputs* replay_inputs = nullptr;

bool ReplayFlatBuf(std::vector<u32>& flat, u32 size_dw) {
    flat.assign(replay_inputs->flat.begin(), replay_inputs->flat.end());
    if (flat.size() == size_dw) {
        return true;
    }
    flat.resize(size_dw);
    Vulkan::replay_flat_ok = false;
    return false;
}

namespace Gcn {

AmdGpu::Buffer ReplayAttribSharp(u8 semantic) {
    AmdGpu::Buffer buffer{};
    buffer.base_address = 1;
    buffer.data_format = static_cast<u32>(AmdGpu::DataFormat::Format32_32_32_32);
    for (const auto& attrib : replay_inputs->attribs) {
        if (attrib.semantic != semantic) {
            continue;
        }
        buffer.num_format = static_cast<u32>(
            attrib.num_class == AmdGpu::NumberClass::Uint   ? AmdGpu::NumberFormat::Uint
            : attrib.num_class == AmdGpu::NumberClass::Sint ? AmdGpu::NumberFormat::Sint
                                                            : AmdGpu::NumberFormat::Float);
        buffer.dst_sel_x = static_cast<u32>(attrib.dst_select.r);
        buffer.dst_sel_y = static_cast<u32>(attrib.dst_select.g);
        buffer.dst_sel_z = static_cast<u32>(attrib.dst_select.b);
        buffer.dst_sel_w = static_cast<u32>(attrib.dst_select.a);
        break;
    }
    return buffer;
}

} // namespace Gcn

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        // bbport: the size is checked before the code is registered (it becomes executable):
        // a cut-short file must not have bytes past its end run as the walker.
        const auto code = ar.CurrPtr();
        ar.Advance(walker_func_size);
        // bbport: earlier builds patched a load that faulted on a null table into `xor rdi, rdi`
        // or `xor r10d, r10d`, and a walker patched on its first walk was stored so: it read
        // empty sharps for good. The walker code has no such xor of its own; the stage is
        // compiled again (a false match only costs that).
        const std::span<const u8> bytes{reinterpret_cast<const u8*>(code), walker_func_size};
        static constexpr std::array<u8, 3> patched_rdi{0x48, 0x31, 0xFF};
        static constexpr std::array<u8, 3> patched_r10d{0x45, 0x31, 0xD2};
        if (!std::ranges::search(bytes, patched_rdi).empty() ||
            !std::ranges::search(bytes, patched_r10d).empty()) {
            walker_func = nullptr;
            return false;
        }
        walker_func = RegisterWalkerCode(code, walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
