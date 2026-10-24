// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_indirect_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/thread.h"
#include "core/memory.h"

namespace Vulkan::IndirectTrace {

namespace {

constexpr u32 PageShift = 12;
constexpr u32 HistoryItems = 32;
constexpr u32 NumRecords = 4096;
constexpr u32 MaxPages = 8192;
constexpr u32 GuestBytes = 32;
constexpr u32 MaxThreads = 256;

struct Item {
    s64 ns;
    VAddr address;
    u64 size;
    u64 detail;
    u64 aux; ///< copies: the source's state (NoteCopy)
    u32 extra;
    u16 thread;
    Event event;
};

struct History {
    std::array<Item, HistoryItems> items{};
    u32 count = 0; ///< items written in all (the ring's head)
};

struct Record {
    u32 seq = ~0u;
    VAddr address = 0;
    u32 bytes = 0;
    bool draw = false;
    bool gpu_modified = false;
    bool in_place = false;
    u64 hash = 0;
    s64 ns = 0;
    u8 guest[GuestBytes]{};
    u32 num_items = 0;
    std::array<Item, HistoryItems * 4> items{};
};

std::mutex mutex;
std::map<u64, History> pages; ///< watched pages
std::vector<Record> records;
std::atomic<u64> watch_first{~0ull}, watch_last{0};
std::atomic<u32> next_seq{0};
std::atomic<u32> rejected{0};
bool pages_full = false;

std::atomic<u32> next_thread{1};
std::array<std::array<char, 24>, MaxThreads> thread_names{};

s64 NowNs() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

/// A small number for the calling thread. Its name is taken on the first event that is not a
/// write fault (a fault handler should not allocate); threads seen only there stay unnamed.
u16 ThreadIndex(Event event) {
    thread_local u32 index = 0;
    thread_local bool named = false;
    if (index == 0) {
        index = next_thread.fetch_add(1, std::memory_order_relaxed);
    }
    if (!named && event != Event::GuestWrite && index < MaxThreads) {
        named = true;
        const std::string name = Common::GetCurrentThreadName();
        std::snprintf(thread_names[index].data(), thread_names[index].size(), "%s", name.c_str());
    }
    return static_cast<u16>(std::min<u32>(index, MaxThreads - 1));
}

const char* ThreadName(u16 index) {
    return thread_names[index][0] ? thread_names[index].data() : "a guest thread";
}

void Describe(const Item& item, char* out, size_t size) {
    const auto range_end = item.address + item.size;
    switch (item.event) {
    case Event::ShaderWrite:
        std::snprintf(out, size, "shader %016llx binds %#llx-%#llx for writing (stride %u)",
                      (unsigned long long)item.detail, (unsigned long long)item.address,
                      (unsigned long long)range_end, item.extra);
        break;
    case Event::Fill:
        std::snprintf(out, size, "DMA fill %#llx-%#llx with %#llx (%s)",
                      (unsigned long long)item.address, (unsigned long long)range_end,
                      (unsigned long long)item.detail, item.extra ? "GPU" : "CPU");
        break;
    case Event::Copy:
        std::snprintf(out, size,
                      "copy into %#llx-%#llx from %#llx (%s; the source: game's memory %#x%s%s%s%s)",
                      (unsigned long long)item.address, (unsigned long long)range_end,
                      (unsigned long long)item.detail, item.extra ? "GPU" : "CPU",
                      u32(item.aux), (item.aux >> 32) & SourceGpuModified ? ", GPU-modified" : "",
                      (item.aux >> 32) & SourceImage ? ", an image there" : "",
                      (item.aux >> 32) & SourceInPlace ? ", in place" : "",
                      (item.aux >> 32) & SourceTwin ? ", twinned" : "");
        break;
    case Event::CommandWrite:
        std::snprintf(out, size, "command write %#llx-%#llx on the CPU, first dword %#llx",
                      (unsigned long long)item.address, (unsigned long long)range_end,
                      (unsigned long long)item.detail);
        break;
    case Event::GpuCommandWrite:
        std::snprintf(out, size, "command write %#llx-%#llx by the GPU, first dword %#llx",
                      (unsigned long long)item.address, (unsigned long long)range_end,
                      (unsigned long long)item.detail);
        break;
    case Event::GuestWrite:
        std::snprintf(out, size, "CPU write %#llx-%#llx (%s)", (unsigned long long)item.address,
                      (unsigned long long)range_end,
                      item.extra == 2   ? "fault window: pages marked CPU-modified"
                      : item.extra == 1 ? "from a command"
                                        : "write fault");
        break;
    case Event::Twin:
        std::snprintf(out, size, "write twin made %#llx-%#llx (%llu GPU bytes on the page)",
                      (unsigned long long)item.address, (unsigned long long)range_end,
                      (unsigned long long)item.detail);
        break;
    case Event::Download:
        std::snprintf(out, size, "GPU data copied back into the game's memory %#llx-%#llx",
                      (unsigned long long)item.address, (unsigned long long)range_end);
        break;
    case Event::Upload:
        std::snprintf(out, size, "upload from the game's memory %#llx-%#llx",
                      (unsigned long long)item.address, (unsigned long long)range_end);
        break;
    case Event::ImageToBuffer:
        std::snprintf(out, size, "image %#llx tiled into the VRAM copy %#llx-%#llx",
                      (unsigned long long)item.detail, (unsigned long long)item.address,
                      (unsigned long long)range_end);
        break;
    case Event::ImageDownload:
        std::snprintf(out, size, "image written back into the game's memory %#llx-%#llx",
                      (unsigned long long)item.address, (unsigned long long)range_end);
        break;
    case Event::Evict:
        std::snprintf(out, size, "VRAM block dropped %#llx-%#llx", (unsigned long long)item.address,
                      (unsigned long long)range_end);
        break;
    case Event::Indirect:
        std::snprintf(out, size, "indirect %s reads %#llx-%#llx (shader %016llx)",
                      item.extra ? "draw" : "dispatch", (unsigned long long)item.address,
                      (unsigned long long)range_end, (unsigned long long)item.detail);
        break;
    }
}

/// The page's history, watched from now on (mutex held); nullptr when no more pages are taken.
History* Watch(u64 page) {
    auto it = pages.find(page);
    if (it != pages.end()) {
        return &it->second;
    }
    if (pages.size() >= MaxPages) {
        if (!pages_full) {
            pages_full = true;
            std::printf("Indirect trace: %u pages watched, no more are added\n", MaxPages);
        }
        return nullptr;
    }
    it = pages.emplace(page, History{}).first;
    u64 low = watch_first.load(std::memory_order_relaxed);
    while (page < low && !watch_first.compare_exchange_weak(low, page)) {
    }
    u64 high = watch_last.load(std::memory_order_relaxed);
    while (page > high && !watch_last.compare_exchange_weak(high, page)) {
    }
    return &it->second;
}

void Push(History& history, const Item& item) {
    history.items[history.count % HistoryItems] = item;
    ++history.count;
}

/// The items of `history` in order, appended to `out`.
void Collect(const History& history, std::vector<Item>& out) {
    const u32 n = std::min(history.count, HistoryItems);
    for (u32 i = history.count - n; i < history.count; ++i) {
        out.push_back(history.items[i % HistoryItems]);
    }
}

} // namespace

bool Enabled() noexcept {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_INDIRECT_TRACE");
        return env && env[0] == '1';
    }();
    return enabled;
}

void Note(Event event, VAddr address, u64 size, u64 detail, u32 extra) {
    if (!Enabled() || size == 0) {
        return;
    }
    const u64 first = address >> PageShift;
    const u64 last = (address + size - 1) >> PageShift;
    if (last < watch_first.load(std::memory_order_relaxed) ||
        first > watch_last.load(std::memory_order_relaxed)) {
        return;
    }
    const Item item{NowNs(), address, size, detail, 0, extra, ThreadIndex(event), event};
    std::scoped_lock lk{mutex};
    for (auto it = pages.lower_bound(first); it != pages.end() && it->first <= last; ++it) {
        Push(it->second, item);
    }
}

void NoteCopy(VAddr dst, u64 size, VAddr src, bool gpu, u32 source_flags) {
    if (!Enabled() || size == 0) {
        return;
    }
    u32 value = 0;
    if (src != ~0ull) {
        Core::Memory::Instance()->ReadBacking(src, &value, std::min<u64>(size, sizeof(value)));
    }
    const Item item{NowNs(), dst, size, src, u64(source_flags) << 32 | value, gpu ? 1u : 0u,
                    ThreadIndex(Event::Copy), Event::Copy};
    std::scoped_lock lk{mutex};
    // Small copies (counters into indirect arguments): their sources are watched too, from the
    // first one seen, so the history of a source shows who wrote it.
    if (src != ~0ull && size <= 64) {
        Watch(src >> PageShift);
    }
    const u64 first = dst >> PageShift, last = (dst + size - 1) >> PageShift;
    for (auto it = pages.lower_bound(first); it != pages.end() && it->first <= last; ++it) {
        Push(it->second, item);
    }
}

u32 OnIndirect(VAddr address, u32 bytes, bool draw, u64 shader_hash, bool gpu_modified,
               bool in_place) {
    if (!Enabled()) {
        return 0;
    }
    const u32 seq = next_seq.fetch_add(1, std::memory_order_relaxed);
    const u64 page = address >> PageShift;
    Record record;
    record.seq = seq;
    record.address = address;
    record.bytes = bytes;
    record.draw = draw;
    record.gpu_modified = gpu_modified;
    record.in_place = in_place;
    record.hash = shader_hash;
    record.ns = NowNs();
    // The game's own bytes there now (through the backing view: the page may be protected).
    Core::Memory::Instance()->ReadBacking(address, record.guest, GuestBytes);
    {
        std::scoped_lock lk{mutex};
        if (records.empty()) {
            records.resize(NumRecords);
        }
        std::vector<Item> items;
        for (const u64 watched : {page - 1, page}) {
            if (const History* history = Watch(watched)) {
                Collect(*history, items);
            }
        }
        // The sources of the copies into the arguments: their pages' history too (the newest
        // three sources).
        std::vector<u64> sources;
        for (auto it = items.rbegin(); it != items.rend() && sources.size() < 3; ++it) {
            if (it->event == Event::Copy && it->detail != ~0ull &&
                it->address < address + bytes && address < it->address + it->size) {
                const u64 source_page = it->detail >> PageShift;
                if (source_page != page && source_page != page - 1 &&
                    std::find(sources.begin(), sources.end(), source_page) == sources.end()) {
                    sources.push_back(source_page);
                }
            }
        }
        for (const u64 source_page : sources) {
            if (const auto it = pages.find(source_page); it != pages.end()) {
                Collect(it->second, items);
            }
        }
        // Both pages see writes that span them: once each, in time order.
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) { return a.ns < b.ns; });
        items.erase(std::unique(items.begin(), items.end(),
                                [](const Item& a, const Item& b) {
                                    return a.ns == b.ns && a.address == b.address &&
                                           a.size == b.size && a.event == b.event;
                                }),
                    items.end());
        const size_t keep = std::min(items.size(), record.items.size());
        std::copy(items.end() - keep, items.end(), record.items.begin());
        record.num_items = static_cast<u32>(keep);
        records[seq % NumRecords] = record;
    }
    Note(Event::Indirect, address, bytes, shader_hash, draw ? 1 : 0);
    return seq;
}

void OnRejected(u32 seq, const u32* args, u32 words, const u32* around, u32 around_words,
                u32 around_from) {
    const u32 n = rejected.fetch_add(1, std::memory_order_relaxed);
    if (n >= 40 && n % 200 != 0) {
        return;
    }
    Record record;
    {
        std::scoped_lock lk{mutex};
        if (!records.empty()) {
            record = records[seq % NumRecords];
        }
    }
    std::string text;
    char line[512];
    if (record.seq != seq) {
        std::snprintf(line, sizeof(line),
                      "Indirect trace: command #%u stopped (%u in all); its record is gone\n", seq,
                      n + 1);
        std::fputs(line, stdout);
        return;
    }
    std::snprintf(line, sizeof(line),
                  "Indirect trace: %s #%u stopped (%u in all): arguments at %#llx, shader "
                  "%016llx; %s, %s\n",
                  record.draw ? "draw" : "dispatch", seq, n + 1,
                  (unsigned long long)record.address, (unsigned long long)record.hash,
                  record.in_place ? "used in place"
                  : record.gpu_modified ? "read from the VRAM copy (GPU-written)"
                                        : "copied from the game's memory (stream buffer)",
                  record.gpu_modified ? "GPU-modified" : "not GPU-modified");
    text += line;
    text += "  the GPU read:";
    for (u32 i = 0; i < words; ++i) {
        float f;
        std::memcpy(&f, &args[i], sizeof(f));
        std::snprintf(line, sizeof(line), " %u (%#x, %g)", args[i], args[i], f);
        text += line;
    }
    text += "\n  the game's memory when recorded:";
    for (u32 i = 0; i < std::min<u32>(words, GuestBytes / 4); ++i) {
        u32 v;
        std::memcpy(&v, record.guest + i * 4, sizeof(v));
        std::snprintf(line, sizeof(line), " %#x", v);
        text += line;
    }
    if (around_words != 0) {
        std::snprintf(line, sizeof(line), "\n  on the GPU from %u bytes before:", around_from);
        text += line;
        for (u32 i = 0; i < around_words; ++i) {
            std::snprintf(line, sizeof(line), "%s%08x", i % 8 == 0 ? "\n    " : " ", around[i]);
            text += line;
        }
    }
    text += "\n  history of its page, the one before and the sources copied into it (ms before "
            "the command, newest last):\n";
    for (u32 i = 0; i < record.num_items; ++i) {
        const Item& item = record.items[i];
        char what[256];
        Describe(item, what, sizeof(what));
        std::snprintf(line, sizeof(line), "    %9.3f  %s [%s]\n",
                      double(record.ns - item.ns) / 1e6, what, ThreadName(item.thread));
        text += line;
    }
    std::fputs(text.c_str(), stdout);
}

} // namespace Vulkan::IndirectTrace
