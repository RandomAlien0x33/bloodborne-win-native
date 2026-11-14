// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/types.h"

/// bbport: the pipeline list shipped with the port (tools/tour/make_pipeline_list.py): the
/// shader descriptions and pipeline keys the game uses, without SPIR-V (it depends on the GPU)
/// and without the shaders' code (the game's own, read from the player's copy). At start the
/// pipeline cache compiles them for this GPU once, so new areas do not stutter on first sight.
namespace Vulkan::PipelineList {

struct Entry {
    u8 type; ///< Storage::BlobType: ShaderMeta or PipelineKey
    std::string name;
    std::vector<u8> data;
};

/// BB_PIPELINE_LIST, or pipeline-list.bin next to the executable.
std::filesystem::path Path();

/// The list's entries and an id of its content; false when there is no list or it is damaged.
bool Read(const std::filesystem::path& path, std::vector<Entry>& entries, u64& id);

/// Shader programs by hash from the game's files (the shader archives and the executable).
/// A geometry shader's copy shader has the geometry shader's hash, so copies are kept apart.
struct GameCode {
    std::unordered_map<u64, std::vector<u32>> programs;
    std::unordered_map<u64, std::vector<u32>> copies;
};
GameCode ReadGameShaders(const std::unordered_set<u64>& wanted);

} // namespace Vulkan::PipelineList
