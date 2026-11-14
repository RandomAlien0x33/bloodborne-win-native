// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <SDL3/SDL_filesystem.h>
#include <miniz.h>
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_pipeline_list.h"

extern "C" int runtime_file_translate(const char* guest, char* out, size_t size);

namespace Vulkan::PipelineList {

namespace {

constexpr u32 Format = 1;
constexpr size_t HeaderSize = 20; // "BBPL", format, entry count (u32 each), payload size (u64)

std::vector<u8> ReadFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return {};
    }
    return {std::istreambuf_iterator<char>(file), {}};
}

template <typename T>
T Get(const std::vector<u8>& data, size_t at) {
    T value{};
    std::memcpy(&value, data.data() + at, sizeof(T));
    return value;
}

u32 BigEndian32(const std::vector<u8>& data, size_t at) {
    return u32(data[at]) << 24 | u32(data[at + 1]) << 16 | u32(data[at + 2]) << 8 | data[at + 3];
}

size_t Find(const std::vector<u8>& data, const char* text, size_t from = 0) {
    const auto end = data.end();
    const auto it = std::search(data.begin() + std::min(from, data.size()), end, text,
                                text + std::strlen(text));
    return it == end ? std::string::npos : size_t(it - data.begin());
}

/// A DCX file's content (zlib after its "DCA" chunk, the size in its "DCS" chunk), anything
/// else as it is.
std::vector<u8> Unpack(std::vector<u8>&& data) {
    if (data.size() < 4 || std::memcmp(data.data(), "DCX\0", 4) != 0) {
        return std::move(data);
    }
    const size_t sizes = Find(data, "DCS");
    const size_t start = Find(data, "DCA");
    if (sizes == std::string::npos || start == std::string::npos || sizes + 8 > data.size() ||
        start + 8 > data.size()) {
        return {};
    }
    std::vector<u8> out(BigEndian32(data, sizes + 4));
    mz_ulong size = out.size();
    if (mz_uncompress(out.data(), &size, data.data() + start + 8,
                      mz_ulong(data.size() - start - 8)) != MZ_OK) {
        return {};
    }
    out.resize(size);
    return out;
}

/// Every program in `data` with an "OrbShdr" header (AmdGpu::BinaryInfo): its code starts with
/// `s_mov_b32 vcc_hi, offset to the header` and is the header's `length` bytes long.
void Collect(const std::vector<u8>& data, const std::unordered_set<u64>& wanted, GameCode& out) {
    constexpr u32 MovVccHi = 0xBEEB03FF;
    constexpr u32 CopyShaderType = 6;
    for (size_t at = Find(data, "OrbShdr"); at != std::string::npos;
         at = Find(data, "OrbShdr", at + 1)) {
        if (at + 32 > data.size()) {
            break;
        }
        const u64 hash = Get<u64>(data, at + 16);
        if (!wanted.contains(hash)) {
            continue;
        }
        const u32 bits = Get<u32>(data, at + 8);
        const u32 length = bits >> 8;
        const bool copy = ((bits >> 2) & 0xF) == CopyShaderType;
        auto& programs = copy ? out.copies : out.programs;
        if (programs.contains(hash)) {
            continue;
        }
        for (size_t back = 0; back <= 0x40000 && back <= at; back += 8) {
            const size_t start = at - back;
            if (Get<u32>(data, start) != MovVccHi ||
                (size_t(Get<u32>(data, start + 4)) + 1) * 8 != back) {
                continue;
            }
            if (start + length <= data.size()) {
                std::vector<u32> code(length / sizeof(u32));
                std::memcpy(code.data(), data.data() + start, code.size() * sizeof(u32));
                programs.emplace(hash, std::move(code));
            }
            break;
        }
    }
}

std::filesystem::path GamePath(const char* guest) {
    char host[1024];
    if (runtime_file_translate(guest, host, sizeof(host)) != 0) {
        return {};
    }
    return std::filesystem::u8path(host);
}

} // namespace

std::filesystem::path Path() {
    if (const char* path = std::getenv("BB_PIPELINE_LIST")) {
        return std::filesystem::u8path(path);
    }
    const char* base = SDL_GetBasePath();
    return std::filesystem::u8path(base ? base : "") / "pipeline-list.bin";
}

bool Read(const std::filesystem::path& path, std::vector<Entry>& entries, u64& id) {
    const auto file = ReadFile(path);
    if (file.empty()) {
        return false;
    }
    if (file.size() < HeaderSize || std::memcmp(file.data(), "BBPL", 4) != 0 ||
        Get<u32>(file, 4) != Format) {
        LOG_WARNING(Render, "Pipeline list {}: not a list this build reads", path.string());
        return false;
    }
    const u32 count = Get<u32>(file, 8);
    std::vector<u8> payload(Get<u64>(file, 12));
    mz_ulong size = payload.size();
    if (mz_uncompress(payload.data(), &size, file.data() + HeaderSize,
                      mz_ulong(file.size() - HeaderSize)) != MZ_OK ||
        size != payload.size()) {
        LOG_WARNING(Render, "Pipeline list {}: damaged", path.string());
        return false;
    }
    entries.clear();
    entries.reserve(count);
    size_t at = 0;
    for (u32 i = 0; i < count; ++i) {
        if (at + 2 > payload.size()) {
            break;
        }
        Entry entry{.type = payload[at]};
        const size_t name_size = payload[at + 1];
        at += 2;
        if (at + name_size + 4 > payload.size()) {
            break;
        }
        entry.name.assign(reinterpret_cast<const char*>(payload.data() + at), name_size);
        at += name_size;
        const u32 data_size = Get<u32>(payload, at);
        at += 4;
        if (at + data_size > payload.size()) {
            break;
        }
        entry.data.assign(payload.begin() + at, payload.begin() + at + data_size);
        at += data_size;
        entries.push_back(std::move(entry));
    }
    if (entries.size() != count) {
        LOG_WARNING(Render, "Pipeline list {}: damaged", path.string());
        return false;
    }
    // FNV-1a of the file: tells this list from another one already imported.
    id = 0xcbf29ce484222325ull;
    for (const u8 byte : file) {
        id = (id ^ byte) * 0x100000001b3ull;
    }
    return true;
}

GameCode ReadGameShaders(const std::unordered_set<u64>& wanted) {
    GameCode out;
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    const auto shaders = GamePath("/app0/dvdroot_ps4/shader");
    if (!shaders.empty()) {
        for (const auto& entry : std::filesystem::directory_iterator{shaders, ec}) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".dcx") {
                files.push_back(entry.path());
            }
        }
    }
    // Shaders the executable carries (clears, copies and the like).
    files.push_back(GamePath("/app0/eboot.bin"));
    for (const auto& file : files) {
        if (!file.empty()) {
            Collect(Unpack(ReadFile(file)), wanted, out);
        }
    }
    return out;
}

} // namespace Vulkan::PipelineList
