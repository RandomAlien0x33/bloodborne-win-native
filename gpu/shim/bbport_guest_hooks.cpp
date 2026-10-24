// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_guest_hooks.h"
// bbport (Windows): a Linux-only diagnostic (signals, /proc, process_vm_readv, dma-buf or
// mprotect traps); off unless its variables are set. Windows builds the inert API below.
#ifndef _WIN32
#include "bbport_heap_sites.h"
#include "bbport_toggles.h"

#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

extern "C" void runtime_memory_note_write(uintptr_t address, uint64_t size);

namespace BbGuestHooks {
namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;
constexpr u64 ImageBase = 0x800000000ull;

/// The return of the game's GPU memory range allocator 0x26aa070 (rdi allocator, rsi size,
/// rdx alignment; it returns a node: [node + 8] the start, [[node + 0x18] + 8] the end, the next
/// node's start): `add rsp, 0x18` in its epilogue, reached by its own branches only.
constexpr u64 AllocReturn = 0x26aa255;
constexpr unsigned char AllocReturnCode[4] = {0x48, 0x83, 0xc4, 0x18};
RangeCallback on_range_allocated = nullptr;

/// The game's other GPU memory heap (TLSF-like, headers in the memory: free at 0x20858a0, alloc at
/// 0x2085a60), reached through a pointer at 0x54b9f38 set at startup. Its only three callers
/// (wrappers that record {address, size} in a map) pass r8 = rbp - 0x50 for the size handed out and
/// follow the call with `mov rbx, rax` (rax: the address).
constexpr std::array<u64, 3> HeapAllocReturns = {0x11e6629, 0x11e6736, 0x11e6846};
constexpr unsigned char HeapAllocReturnCode[3] = {0x48, 0x89, 0xc3};

/// BB_FRAME_STATS: the range allocator's collector (0x26aa860, `push rbp`; rdi the allocator): its
/// live range count [+0xc8] and byte count [+0xd0] are noted for the frame stats.
constexpr u64 CollectorEntry = 0x26aa860;
constexpr unsigned char CollectorEntryCode[1] = {0x55};

/// BB_HEAP_SITES=1: the game's deferred object release (0xbb5f10) skips the destruction when the
/// registry check (0xbf0790) fails: `js 0xbb5fc7` there, its result counted (eax).
constexpr u64 ReleaseCheck = 0xbb5fb0;
constexpr unsigned char ReleaseCheckCode[2] = {0x78, 0x15};

/// `call memcpy` (0x261a1b0: rdi destination, rsi source, rdx size) in the resource loaders:
/// 0x216d420 and 0x2171d60 copy texture and buffer data into GPU memory (most of the write
/// faults while areas stream in).
struct CallSite {
    u64 offset;
    unsigned char code[5];
};
constexpr std::array<CallSite, 2> MemcpySites = {{
    {0x216d5ea, {0xe8, 0xc1, 0xcb, 0x4a, 0x00}},
    {0x2171fea, {0xe8, 0xc1, 0x81, 0x4a, 0x00}},
}};
std::array<std::atomic<u64>, MemcpySites.size()> hits{};

/// The game's memcpy at those sites: the copy, then the GPU side is told of the data now there.
void* LoaderCopy(void* dst, const void* src, std::size_t size) {
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    std::memcpy(dst, src, size);
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    return dst;
}

/// bbport: native detours (default; BB_GUEST_HOOK_TRAPS=1: int3 traps). Each hooked site jumps to
/// a stub next to the image that calls the handler and runs the instructions the jump covers;
/// the copy call sites call LoaderCopy through a trampoline there. An int3 trap costs a signal
/// (a few microseconds in the kernel) per call; a detour costs a call. The int3 written first
/// stays the fallback while the jump's bytes go in: OnTrap sends a thread that meets it to the stub.
struct NativeSite {
    u64 offset;
    std::atomic<u64> stub{0};
};
std::array<NativeSite, 4> native_sites{{{AllocReturn}, {HeapAllocReturns[0]}, {HeapAllocReturns[1]},
                                        {HeapAllocReturns[2]}}};
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 4096;

/// A page within +-2 GiB of the image (below its base: the image grows upwards).
u8* MapStubPage() {
    for (u64 k = 1; k <= 64; ++k) {
        const u64 hint = ImageBase - k * (16ull << 20);
        void* p = mmap(reinterpret_cast<void*>(hint), StubPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) {
            return static_cast<u8*>(p);
        }
    }
    return nullptr;
}

struct Emitter {
    std::array<u8, 256> code{};
    std::size_t size = 0;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            code[size++] = b;
        }
    }
    void U32(std::uint32_t v) {
        std::memcpy(&code[size], &v, 4);
        size += 4;
    }
    void U64(u64 v) {
        std::memcpy(&code[size], &v, 8);
        size += 8;
    }
    /// Saves the caller-saved registers (the handler may change them), aligns the stack.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12 (holds the unaligned stack pointer)
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0}); // call rax
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
};

/// Copies `e` into the stub page; returns its address there, or 0.
u64 PlaceStub(const Emitter& e) {
    if (!stub_page || stub_used + e.size > StubPageSize) {
        return 0;
    }
    u8* at = stub_page + stub_used;
    std::memcpy(at, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    return reinterpret_cast<u64>(at);
}

bool Rel32To(u64 from_next, u64 to, std::uint32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = std::uint32_t(std::int32_t(d));
    return true;
}

/// Replaces bytes [1, 5) at `address` (byte 0 is the trap already there), then byte 0.
void WriteJumpOverTrap(u64 address, u8 opcode, std::uint32_t rel) {
    const long page_size = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    const std::size_t span = (address + 5) - reinterpret_cast<u64>(page);
    mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
    auto* p = reinterpret_cast<u8*>(address);
    for (int i = 0; i < 4; ++i) {
        __atomic_store_n(p + 1 + i, u8(rel >> (8 * i)), __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(p, opcode, __ATOMIC_SEQ_CST);
    mprotect(page, span, PROT_READ | PROT_EXEC);
}

void AllocHook(u64 node) {
    if (!node) {
        return;
    }
    const u64 start = *reinterpret_cast<const u64*>(node + 8);
    const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
    const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
    if (end > start && end - start < (u64(1) << 32)) {
        on_range_allocated(start, end - start);
    }
}

void HeapHook(u64 address, u64 size) {
    if (address != 0 && size != 0 && size < (u64(1) << 32)) {
        on_range_allocated(address, size);
    }
}

/// The detour of a site whose int3 is in place (Patch). False: left as a trap.
bool InstallDetour(NativeSite& site) {
    const u64 address = ImageBase + site.offset;
    Emitter e;
    u64 back = 0;
    if (site.offset == AllocReturn) {
        // `add rsp, 0x18; pop rbx` (5 bytes) in the allocator's epilogue; rax: the node.
        if (reinterpret_cast<const u8*>(address)[4] != 0x5b) {
            return false;
        }
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xc7}); // mov rdi, rax (unchanged by the saving)
        e.CallAbs(reinterpret_cast<const void*>(&AllocHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x83, 0xc4, 0x18}); // add rsp, 0x18
        e.Bytes({0x5b});                   // pop rbx
        back = address + 5;
    } else {
        // `mov rbx, rax; test rbx, rbx` (6 bytes) after the heap call; [rbp - 0x50]: the size.
        const auto* code = reinterpret_cast<const u8*>(address);
        if (code[3] != 0x48 || code[4] != 0x85 || code[5] != 0xdb) {
            return false;
        }
        e.Bytes({0x48, 0x89, 0xc3});                               // mov rbx, rax
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); // lea rsp, [rsp - 128]
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xdf});       // mov rdi, rbx
        e.Bytes({0x48, 0x8b, 0x75, 0xb0}); // mov rsi, [rbp - 0x50]
        e.CallAbs(reinterpret_cast<const void*>(&HeapHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
        e.Bytes({0x48, 0x85, 0xdb});                               // test rbx, rbx
        back = address + 6;
    }
    // jmp back
    const u64 jmp_at = reinterpret_cast<u64>(stub_page + stub_used) + e.size;
    std::uint32_t rel_back = 0;
    if (!Rel32To(jmp_at + 5, back, rel_back)) {
        return false;
    }
    e.Bytes({0xe9});
    e.U32(rel_back);
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    site.stub.store(stub, std::memory_order_release); // a thread at the trap now goes there
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The copy call sites: the call's target becomes a trampoline to LoaderCopy.
bool InstallCallDetour(u64 offset) {
    Emitter e;
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(reinterpret_cast<u64>(&LoaderCopy));
    const u64 stub = PlaceStub(e);
    const u64 address = ImageBase + offset;
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe8, rel);
    return true;
}
struct sigaction previous_action {};

void OnTrap(int sig, siginfo_t* info, void* context) {
    auto* g = static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
    const u64 rip = u64(g[REG_RIP]) - 1; // past the int3
    for (const auto& site : native_sites) {
        if (rip == ImageBase + site.offset) {
            if (const u64 stub = site.stub.load(std::memory_order_acquire)) {
                g[REG_RIP] = greg_t(stub); // the detour is going in: its stub runs the site
                return;
            }
        }
    }
    if (rip == ImageBase + ReleaseCheck) {
        const auto result = static_cast<std::int32_t>(g[REG_RAX] & 0xffffffff);
        BbHeapSites::NoteReleaseCheck(std::uint32_t(result));
        g[REG_RIP] = greg_t(ImageBase + (result < 0 ? 0xbb5fc7 : ReleaseCheck + 2));
        return;
    }
    if (rip == ImageBase + CollectorEntry) {
        const u64 allocator = u64(g[REG_RDI]);
        for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
            u64 expected = 0;
            if (BbStats::range_allocators[i].load(std::memory_order_relaxed) == allocator ||
                BbStats::range_allocators[i].compare_exchange_strong(expected, allocator)) {
                BbStats::range_live[i].store(*reinterpret_cast<const u64*>(allocator + 0xc8),
                                             std::memory_order_relaxed);
                BbStats::range_bytes[i].store(*reinterpret_cast<const u64*>(allocator + 0xd0),
                                              std::memory_order_relaxed);
                break;
            }
        }
        g[REG_RSP] -= 8; // push rbp
        *reinterpret_cast<u64*>(g[REG_RSP]) = u64(g[REG_RBP]);
        g[REG_RIP] = greg_t(ImageBase + CollectorEntry + 1);
        return;
    }
    for (const u64 site : HeapAllocReturns) {
        if (rip != ImageBase + site) {
            continue;
        }
        const u64 address = u64(g[REG_RAX]);
        const u64 size = *reinterpret_cast<const u64*>(u64(g[REG_RBP]) - 0x50);
        if (address != 0 && size != 0 && size < (u64(1) << 32)) {
            on_range_allocated(address, size);
        }
        g[REG_RBX] = g[REG_RAX];
        g[REG_RIP] = greg_t(ImageBase + site + sizeof(HeapAllocReturnCode));
        return;
    }
    if (rip == ImageBase + AllocReturn) {
        if (const u64 node = u64(g[REG_RAX])) {
            const u64 start = *reinterpret_cast<const u64*>(node + 8);
            const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
            const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
            if (end > start && end - start < (u64(1) << 32)) {
                on_range_allocated(start, end - start);
            }
        }
        g[REG_RSP] += 0x18;
        g[REG_RIP] = greg_t(ImageBase + AllocReturn + sizeof(AllocReturnCode));
        return;
    }
    for (std::size_t i = 0; i < MemcpySites.size(); ++i) {
        const auto& site = MemcpySites[i];
        if (rip != ImageBase + site.offset) {
            continue;
        }
        hits[i].fetch_add(1, std::memory_order_relaxed);
        // The call, to LoaderCopy instead of the game's memcpy: push the return address, jump.
        const u64 ret = ImageBase + site.offset + 5;
        g[REG_RSP] -= 8;
        *reinterpret_cast<u64*>(g[REG_RSP]) = ret;
        g[REG_RIP] = greg_t(reinterpret_cast<u64>(&LoaderCopy));
        return;
    }
    if (previous_action.sa_flags & SA_SIGINFO) {
        if (previous_action.sa_sigaction) {
            previous_action.sa_sigaction(sig, info, context);
            return;
        }
    } else if (previous_action.sa_handler != SIG_DFL && previous_action.sa_handler != SIG_IGN &&
               previous_action.sa_handler) {
        previous_action.sa_handler(sig);
        return;
    }
    signal(SIGTRAP, SIG_DFL);
    raise(SIGTRAP);
}

bool Patch(u64 address, const unsigned char* expected, std::size_t size) {
    unsigned char bytes[16]{};
    iovec local{bytes, size}, remote{reinterpret_cast<void*>(address), size};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != ssize_t(size) ||
        std::memcmp(bytes, expected, size) != 0) {
        return false;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    const std::size_t span = (address + size) - reinterpret_cast<u64>(page);
    if (mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<unsigned char*>(address), static_cast<unsigned char>(0xcc),
                     __ATOMIC_SEQ_CST);
    mprotect(page, span, PROT_READ | PROT_EXEC);
    return true;
}
} // namespace

void Install(RangeCallback on_gpu_range_allocated) {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) {
        return;
    }
    struct sigaction action {};
    action.sa_sigaction = OnTrap;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTRAP, &action, &previous_action);
    int patched = 0;
    for (const auto& site : MemcpySites) {
        patched += Patch(ImageBase + site.offset, site.code, sizeof(site.code));
    }
    std::printf("Guest hooks: %d of %zu resource copy sites tell the GPU side of their writes\n",
                patched, MemcpySites.size());
    on_range_allocated = on_gpu_range_allocated;
    const bool allocator = Patch(ImageBase + AllocReturn, AllocReturnCode, sizeof(AllocReturnCode));
    // BB_HEAP_HOOKS=0 (bisecting): the heap's ranges are not reported.
    const char* heap_env = std::getenv("BB_HEAP_HOOKS");
    const bool heap_hooks = !heap_env || heap_env[0] != '0';
    if (BbStats::enabled) {
        Patch(ImageBase + CollectorEntry, CollectorEntryCode, sizeof(CollectorEntryCode));
    }
    int heap_sites = 0;
    for (const u64 site : HeapAllocReturns) {
        if (!heap_hooks) {
            break;
        }
        heap_sites += Patch(ImageBase + site, HeapAllocReturnCode, sizeof(HeapAllocReturnCode));
    }
    std::printf("Guest hooks: GPU heap allocations seen at %d of %zu call sites\n", heap_sites,
                HeapAllocReturns.size());
    std::printf("Guest hooks: GPU memory allocator %s\n", allocator ? "tells the GPU side of new ranges" : "not found");
    // Native detours over the traps just placed (BB_GUEST_HOOK_TRAPS=1 keeps the traps).
    const char* traps_env = std::getenv("BB_GUEST_HOOK_TRAPS");
    if (!(traps_env && traps_env[0] == '1') && (stub_page = MapStubPage()) != nullptr) {
        int detours = 0;
        for (const auto& site : MemcpySites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallCallDetour(site.offset);
            }
        }
        for (auto& site : native_sites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallDetour(site);
            }
        }
        mprotect(stub_page, StubPageSize, PROT_READ | PROT_EXEC);
        std::printf("Guest hooks: %d native detours (no traps; BB_GUEST_HOOK_TRAPS=1: int3)\n",
                    detours);
    }
    BbHeapSites::Install();
    if (const char* env = std::getenv("BB_HEAP_SITES"); env && env[0] == '1') {
        Patch(ImageBase + ReleaseCheck, ReleaseCheckCode, sizeof(ReleaseCheckCode));
    }
}

void PatchImage(unsigned char*, std::uint64_t) {}
} // namespace BbGuestHooks
#else
// bbport (Windows): the same sites as native detours, written while the image is patched before
// the game runs (bbgpu_patch_image): no int3 traps and no threads meeting a half-written jump.
// The BufferCache hands its callback over later (Install); allocations before that go unseen,
// as on Linux, where the hooks go in at the same point.
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" void runtime_memory_note_write(uintptr_t address, uint64_t size);
extern "C" void* runtime_low_map(size_t size, int prot);
extern "C" int runtime_extra_direct_map(uint64_t size, uint64_t alignment, void** out);

namespace BbGuestHooks {
namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;
#define GUEST_ABI __attribute__((sysv_abi))

/// See the Linux part above for what each site is.
constexpr u64 AllocReturn = 0x26aa255;
constexpr std::array<u64, 3> HeapAllocReturns = {0x11e6629, 0x11e6736, 0x11e6846};
struct CallSite {
    u64 offset;
    unsigned char code[5];
};
constexpr std::array<CallSite, 2> MemcpySites = {{
    {0x216d5ea, {0xe8, 0xc1, 0xcb, 0x4a, 0x00}},
    {0x2171fea, {0xe8, 0xc1, 0x81, 0x4a, 0x00}},
}};
std::atomic<RangeCallback> on_range_allocated{nullptr};

/// Called from the game's code: its calling convention.
GUEST_ABI void* LoaderCopy(void* dst, const void* src, std::size_t size) {
    // Told before (its write traps lift: no fault per page) and after the copy (the data there).
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    std::memcpy(dst, src, size);
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    return dst;
}

GUEST_ABI void AllocHook(u64 node) {
    const RangeCallback callback = on_range_allocated.load(std::memory_order_acquire);
    if (!callback || !node) {
        return;
    }
    const u64 start = *reinterpret_cast<const u64*>(node + 8);
    const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
    const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
    if (end > start && end - start < (u64(1) << 32)) {
        callback(start, end - start);
    }
}

GUEST_ABI void HeapHook(u64 address, u64 size) {
    const RangeCallback callback = on_range_allocated.load(std::memory_order_acquire);
    if (callback && address != 0 && size != 0 && size < (u64(1) << 32)) {
        callback(address, size);
    }
}

struct Emitter {
    std::array<u8, 256> code{};
    std::size_t size = 0;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            code[size++] = b;
        }
    }
    void U32(std::uint32_t v) {
        std::memcpy(&code[size], &v, 4);
        size += 4;
    }
    void U64(u64 v) {
        std::memcpy(&code[size], &v, 8);
        size += 8;
    }
    /// Saves the registers the handler may change (the game's convention), aligns the stack.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12 (holds the unaligned stack pointer)
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0}); // call rax
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
};

u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 4096;

bool Rel32To(u64 from_next, u64 to, std::uint32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = std::uint32_t(std::int32_t(d));
    return true;
}

/// Places `e` (ending in `jmp back` when back != 0) in the stub page; its address, or 0.
u64 PlaceStub(Emitter& e, u64 back) {
    if (!stub_page || stub_used + e.size + 5 > StubPageSize) {
        return 0;
    }
    u8* at = stub_page + stub_used;
    if (back != 0) {
        std::uint32_t rel = 0;
        if (!Rel32To(reinterpret_cast<u64>(at) + e.size + 5, back, rel)) {
            return 0;
        }
        e.Bytes({0xe9});
        e.U32(rel);
    }
    std::memcpy(at, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    return reinterpret_cast<u64>(at);
}

/// `opcode rel32` to `stub` at `site` (the image is still writable).
bool WriteJump(u8* site, u8 opcode, u64 stub) {
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(reinterpret_cast<u64>(site) + 5, stub, rel)) {
        return false;
    }
    site[0] = opcode;
    std::memcpy(site + 1, &rel, 4);
    return true;
}

bool HookAllocReturn(u8* image) {
    // `add rsp, 0x18; pop rbx` (5 bytes) in the allocator's epilogue; rax: the node.
    u8* site = image + AllocReturn;
    static constexpr u8 Code[5] = {0x48, 0x83, 0xc4, 0x18, 0x5b};
    if (std::memcmp(site, Code, sizeof(Code)) != 0) {
        return false;
    }
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xc7}); // mov rdi, rax
    e.CallAbs(reinterpret_cast<const void*>(&AllocHook));
    e.RestoreVolatile();
    e.Bytes({0x48, 0x83, 0xc4, 0x18}); // add rsp, 0x18
    e.Bytes({0x5b});                   // pop rbx
    return WriteJump(site, 0xe9, PlaceStub(e, reinterpret_cast<u64>(site) + 5));
}

bool HookHeapReturn(u8* image, u64 offset) {
    // `mov rbx, rax; test rbx, rbx` (6 bytes) after the heap call; [rbp - 0x50]: the size.
    u8* site = image + offset;
    static constexpr u8 Code[6] = {0x48, 0x89, 0xc3, 0x48, 0x85, 0xdb};
    if (std::memcmp(site, Code, sizeof(Code)) != 0) {
        return false;
    }
    Emitter e;
    e.Bytes({0x48, 0x89, 0xc3});                               // mov rbx, rax
    e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); // lea rsp, [rsp - 128] (red zone)
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xdf});       // mov rdi, rbx
    e.Bytes({0x48, 0x8b, 0x75, 0xb0}); // mov rsi, [rbp - 0x50]
    e.CallAbs(reinterpret_cast<const void*>(&HeapHook));
    e.RestoreVolatile();
    e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
    e.Bytes({0x48, 0x85, 0xdb});                               // test rbx, rbx
    if (!WriteJump(site, 0xe9, PlaceStub(e, reinterpret_cast<u64>(site) + 6))) {
        return false;
    }
    site[5] = 0xcc; // never reached: the stub jumps back past it
    return true;
}

bool HookCopyCall(u8* image, const CallSite& call) {
    u8* site = image + call.offset;
    if (std::memcmp(site, call.code, sizeof(call.code)) != 0) {
        return false;
    }
    Emitter e;
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(reinterpret_cast<u64>(&LoaderCopy));
    return WriteJump(site, 0xe8, PlaceStub(e, 0));
}

/// The GX swap-chain's per-frame render command (deadinside28's 0.6, #162 #192): its 16-byte node
/// comes from a per-frame bump allocator that returns null once the frame's heap is used up; the
/// game writes through it unchecked (`mov [r12 + 8], r13` at 0x2199310, then posts it to the
/// render thread): a guest fault at address 8. Without a node the post is skipped and the pending
/// flag (this + 0x3ab) stays set, so the command goes out with the next frame.
constexpr u64 GxNodeSite = 0x2199310;
constexpr u64 GxNodeSkip = 0x219934e; // past the post and the flag's clear
bool GuardGxNode(u8* image) {
    u8* site = image + GxNodeSite;
    static constexpr u8 Code[9] = {0x4d, 0x89, 0x6c, 0x24, 0x08,  // mov [r12 + 8], r13
                                   0x49, 0x8b, 0x87, 0xb0};       // mov rax, [r15 + 0xb0]
    if (std::memcmp(site, Code, sizeof(Code)) != 0) {
        return false;
    }
    Emitter e;
    e.Bytes({0x4d, 0x85, 0xe4});             // test r12, r12
    e.Bytes({0x74, 0x0a});                   // jz null (past the next 10 bytes)
    e.Bytes({0x4d, 0x89, 0x6c, 0x24, 0x08}); // mov [r12 + 8], r13
    const std::size_t jmp_back = e.size;
    e.Bytes({0xe9, 0x00, 0x00, 0x00, 0x00}); // jmp back to the next instruction (below)
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // null: jmp [rip], past the post
    e.U64(reinterpret_cast<u64>(image + GxNodeSkip));
    const u64 stub = PlaceStub(e, 0);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(stub + jmp_back + 5, reinterpret_cast<u64>(site) + 5, rel)) {
        return false;
    }
    std::memcpy(reinterpret_cast<u8*>(stub) + jmp_back + 1, &rel, 4);
    return WriteJump(site, 0xe9, stub);
}

/// A render command node from the frame's linear heap (the object [0x55406c8] + 0x360 + 8 * its
/// frame index, allocator at +0x78): 0x4d0 bytes, 0x398 of them copied from the request at once,
/// unchecked. When the frame's heap is used up the allocator returns null and the copy writes at
/// address 0 (ucrtbase memcpy +0xee4b3, write at 0x20: 2026-10-11 after 1 h 45 min, 2026-10-16 at
/// an area load). The allocator's lock is released before the copy: a null node returns from
/// the function (its own epilogue), the command is left out of that frame.
constexpr u64 FrameNodeSite = 0x21d5b35;     // mov edx, 0x398 (the copy's size), after the unlock
constexpr u64 FrameNodeEpilogue = 0x21d5aca; // add rsp, 8; pop rbx, r12-r15, rbp; ret
std::atomic<std::uint32_t> frame_node_skips{0};
GUEST_ABI void FrameNodeSkipped() {
    const std::uint32_t n = frame_node_skips.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || (n & (n - 1)) == 0) {
        std::printf("Guest hooks: the frame's render command heap is full: a command left out "
                    "(%u so far)\n", n);
    }
}
bool GuardFrameNode(u8* image) {
    u8* site = image + FrameNodeSite;
    static constexpr u8 Code[11] = {0xba, 0x98, 0x03, 0x00, 0x00,  // mov edx, 0x398
                                    0x4c, 0x89, 0xef,              // mov rdi, r13
                                    0x48, 0x89, 0xde};             // mov rsi, rbx
    static constexpr u8 Epilogue[15] = {0x48, 0x83, 0xc4, 0x08, 0x5b, 0x41, 0x5c, 0x41,
                                        0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3};
    if (std::memcmp(site, Code, sizeof(Code)) != 0 ||
        std::memcmp(image + FrameNodeEpilogue, Epilogue, sizeof(Epilogue)) != 0) {
        return false;
    }
    Emitter e;
    e.Bytes({0x4d, 0x85, 0xed});             // test r13, r13
    e.Bytes({0x74, 0x0a});                   // jz null (past the next 10 bytes)
    e.Bytes({0xba, 0x98, 0x03, 0x00, 0x00}); // mov edx, 0x398
    const std::size_t jmp_back = e.size;
    e.Bytes({0xe9, 0x00, 0x00, 0x00, 0x00}); // jmp back to the next instruction (below)
    e.SaveVolatile();                        // null: counted and logged
    e.CallAbs(reinterpret_cast<const void*>(&FrameNodeSkipped));
    e.RestoreVolatile();
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]: the function's epilogue
    e.U64(reinterpret_cast<u64>(image + FrameNodeEpilogue));
    const u64 stub = PlaceStub(e, 0);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(stub + jmp_back + 5, reinterpret_cast<u64>(site) + 5, rel)) {
        return false;
    }
    std::memcpy(reinterpret_cast<u8*>(stub) + jmp_back + 1, &rel, 4);
    return WriteJump(site, 0xe9, stub);
}

/// The frame's render memory: two arenas (one per frame, [0x55406c8] + 0x370 + 8 * index), each a
/// single 256 KiB block (0x40000, set at 0x2195c09 and 0x2195c46) taken from the game's heap once
/// at start and reset at each frame swap. The frame's render heaps (+0x360) take their chunks from
/// it, and so do the nodes the two guards above check. The biggest user is the frame's draw list
/// (0x2285c40): 0xb0 bytes + 8 per entry + 0x30 per visible item, copied whole into the frame
/// heap each frame. On 2026-10-18 it reached 0x3f2c0 bytes (258752 of 262144) at an area load:
/// the frame's commands had been left out for minutes before (the guards), then the list itself
/// did not fit and the game copied it to address 0 (its memcpy, guest+0x261af9e). The arena is
/// made bigger (BB_FRAME_ARENA_KB, default 1024 as in deadinside28's 0.6 (PR #224); 256: the
/// game's own size). The guards above stay: a frame that still fills it loses commands, not the
/// game.
constexpr std::array<u64, 2> FrameArenaSizeSites = {0x2195c09, 0x2195c46}; // mov edx, 0x40000
constexpr u64 FrameArenaGlobal = 0x55406c8;
constexpr u64 DrawListSizeSite = 0x2285d3b; // mov r12d, [r15 + 0x80]: the draw list's size
std::uint32_t frame_arena_bytes = 0x40000;
const u8* guest_image = nullptr;

struct ArenaUse {
    u64 used = 0, size = 0;
};
/// A frame's arena (its linear allocator at +8: begin, end, current), or zeros: the current
/// frame's, or the other one: the previous frame, all it took (reset when its turn comes again).
ArenaUse FrameArena(bool previous) {
    const u64 owner = *reinterpret_cast<const u64*>(guest_image + FrameArenaGlobal);
    if (owner == 0) {
        return {};
    }
    const int index = *reinterpret_cast<const int*>(owner + 0x380) ^ (previous ? 1 : 0);
    const u64 arena = *reinterpret_cast<const u64*>(owner + 0x370 + 8 * u64(index & 1));
    if (arena == 0) {
        return {};
    }
    const u64 begin = *reinterpret_cast<const u64*>(arena + 8);
    const u64 end = *reinterpret_cast<const u64*>(arena + 0x10);
    const u64 current = *reinterpret_cast<const u64*>(arena + 0x18);
    return {current - begin, end - begin};
}

std::atomic<std::uint32_t> draw_list_peak{0}, arena_peak{0};
std::atomic<long long> draw_list_report_ms{0};
std::atomic<std::uint32_t> kernel_samples_fixed{0}; // InstallKernelSampleClamp's stub counts here

/// BB_FRAME_ARENA_TRACE=1 (diagnostics): what takes the frame arenas' memory. The arena class
/// (vtable 0x52e6fc0) is a two-ended stack whose front is reset each frame; its allocation methods
/// (0xe45350: size; 0xe453b0: size, alignment) are hooked, and the bytes taken from the two frame
/// arenas are summed per calling site, reported each minute per frame. The frame's command heaps
/// take their chunks there through 0xe45560 (a new chunk of max(chunk size, request) when the
/// current one is full): for those, the site is 0xe45560's caller and the bytes its request.
constexpr u64 CommandHeapChunkSite = 0xe455c2; // in 0xe45560, after the arena's allocation
struct ArenaSite {
    u64 bytes = 0, calls = 0, largest = 0;
    u64 chain[3]{}; // for command heap chunks: three return addresses up from 0xe45560
};
std::mutex arena_sites_mutex;
std::unordered_map<u64, ArenaSite> arena_sites;

/// Return addresses up the guest's frame-pointer chain from `frame`, while they stay in the image.
void WalkFrames(u64 frame, u64* out, int count) {
    const u64 base = reinterpret_cast<u64>(guest_image);
    for (int i = 0; i < count; ++i) {
        MEMORY_BASIC_INFORMATION info{};
        if (frame < 0x10000 || VirtualQuery(reinterpret_cast<const void*>(frame), &info,
                                            sizeof(info)) == 0 ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_READWRITE | PAGE_READONLY)) == 0) {
            return;
        }
        const u64 ret = reinterpret_cast<const u64*>(frame)[1];
        if (ret < base || ret - base > 0x6000000) {
            return;
        }
        out[i] = ret - base;
        frame = reinterpret_cast<const u64*>(frame)[0];
    }
}
std::atomic<u64> arena_trace_frames{0};
bool arena_trace = false;
GUEST_ABI void ArenaAllocated(u64 arena, u64 size, u64 caller, u64 frame, u64 request) {
    const u64 owner = *reinterpret_cast<const u64*>(guest_image + FrameArenaGlobal);
    if (owner == 0 || (arena != *reinterpret_cast<const u64*>(owner + 0x370) &&
                       arena != *reinterpret_cast<const u64*>(owner + 0x378))) {
        return;
    }
    const u64 base = reinterpret_cast<u64>(guest_image);
    u64 chain[3]{};
    u64 key = caller - base;
    if (key == CommandHeapChunkSite) {
        WalkFrames(frame, chain, 3); // 0xe45560's caller and two more
        key = chain[0] ^ (chain[1] << 21) ^ (chain[2] << 42);
        size = request;
    }
    std::scoped_lock lock{arena_sites_mutex};
    ArenaSite& site = arena_sites[key];
    site.bytes += size;
    ++site.calls;
    site.largest = std::max(site.largest, size);
    if (chain[0]) {
        std::memcpy(site.chain, chain, sizeof(chain));
    }
}

void ReportArenaSites() {
    std::vector<std::pair<u64, ArenaSite>> sites;
    {
        std::scoped_lock lock{arena_sites_mutex};
        sites.assign(arena_sites.begin(), arena_sites.end());
        arena_sites.clear();
    }
    const u64 frames = std::max<u64>(arena_trace_frames.exchange(0), 1);
    std::sort(sites.begin(), sites.end(),
              [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    std::printf("Frame arena trace (%llu frames):", (unsigned long long)frames);
    for (size_t i = 0; i < sites.size() && i < 10; ++i) {
        const auto& c = sites[i].second.chain;
        if (c[0]) {
            std::printf(" +%#llx<+%#llx<+%#llx", (unsigned long long)c[0], (unsigned long long)c[1],
                        (unsigned long long)c[2]);
        } else {
            std::printf(" +%#llx", (unsigned long long)sites[i].first);
        }
        std::printf(" %.1f KiB/frame (%.1f calls, largest %.1f KiB);",
                    double(sites[i].second.bytes) / 1024.0 / double(frames),
                    double(sites[i].second.calls) / double(frames),
                    double(sites[i].second.largest) / 1024.0);
    }
    std::printf("\n");
}

GUEST_ABI void DrawListSized(std::uint32_t list_bytes) {
    static std::atomic<bool> checked{false};
    arena_trace_frames.fetch_add(1, std::memory_order_relaxed);
    // The list is copied right after this, early in the frame; the previous frame's arena shows
    // what a whole frame took.
    const ArenaUse arena = FrameArena(false);
    const ArenaUse previous = FrameArena(true);
    if (!checked.exchange(true)) {
        std::printf("Guest hooks: frame render memory %llu KiB per frame (asked for %u KiB)%s\n",
                    (unsigned long long)(arena.size >> 10), frame_arena_bytes >> 10,
                    arena.size >= frame_arena_bytes ? "" : ": NOT GIVEN by the game's heap");
    }
    std::uint32_t seen = draw_list_peak.load(std::memory_order_relaxed);
    while (list_bytes > seen && !draw_list_peak.compare_exchange_weak(seen, list_bytes)) {
    }
    const auto used = std::uint32_t(previous.used);
    seen = arena_peak.load(std::memory_order_relaxed);
    while (used > seen && !arena_peak.compare_exchange_weak(seen, used)) {
    }
    const long long now = GetTickCount64();
    long long last = draw_list_report_ms.load(std::memory_order_relaxed);
    if (last == 0) {
        draw_list_report_ms.compare_exchange_strong(last, now);
    } else if (now - last >= 60000 && draw_list_report_ms.compare_exchange_strong(last, now)) {
        std::printf("Guest hooks: frame render memory: draw list up to %u KiB, a whole frame "
                    "took up to %u of %llu KiB (largest of the minute)\n",
                    draw_list_peak.exchange(0) >> 10, arena_peak.exchange(0) >> 10,
                    (unsigned long long)(arena.size >> 10));
        if (const std::uint32_t fixed = kernel_samples_fixed.load(std::memory_order_relaxed)) {
            std::printf("Guest hooks: blur kernel sample counts corrected %u times so far\n", fixed);
        }
        if (arena_trace) {
            ReportArenaSites();
        }
    }
}

bool PatchFrameArena(u8* image) {
    const char* env = std::getenv("BB_FRAME_ARENA_KB");
    const long kb = env && env[0] ? std::strtol(env, nullptr, 10) : 1024;
    if (kb <= 256 || kb > 65536) {
        return false;
    }
    static constexpr u8 Code[5] = {0xba, 0x00, 0x00, 0x04, 0x00}; // mov edx, 0x40000
    for (const u64 site : FrameArenaSizeSites) {
        if (std::memcmp(image + site, Code, sizeof(Code)) != 0) {
            return false;
        }
    }
    frame_arena_bytes = std::uint32_t(kb) << 10;
    for (const u64 site : FrameArenaSizeSites) {
        std::memcpy(image + site + 1, &frame_arena_bytes, 4);
    }
    return true;
}

bool HookDrawListSize(u8* image) {
    u8* site = image + DrawListSizeSite;
    static constexpr u8 Code[7] = {0x45, 0x8b, 0xa7, 0x80, 0x00, 0x00, 0x00};
    if (std::memcmp(site, Code, sizeof(Code)) != 0) {
        return false;
    }
    Emitter e;
    e.Bytes({0x45, 0x8b, 0xa7, 0x80, 0x00, 0x00, 0x00});             // mov r12d, [r15 + 0x80]
    e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff});       // lea rsp, [rsp - 128]
    e.SaveVolatile();
    // SaveVolatile keeps the stack pointer in r12; the game's r12 (the size) is saved at [r12].
    e.Bytes({0x41, 0x8b, 0x3c, 0x24});                               // mov edi, [r12]
    e.CallAbs(reinterpret_cast<const void*>(&DrawListSized));
    e.RestoreVolatile();
    e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00});       // lea rsp, [rsp + 128]
    if (!WriteJump(site, 0xe9, PlaceStub(e, reinterpret_cast<u64>(site) + 7))) {
        return false;
    }
    site[5] = 0xcc; // never reached: the stub jumps back past them
    site[6] = 0xcc;
    return true;
}

/// Yebis's blur-kernel sample parameters (GPUTexUtil_GetRecursiveSampleParameters, 0xbbc3e0;
/// deadinside28's 0.6, #207): the game asserts (GPUMath.cpp 3199-3216, compiled into the retail
/// code: a crash, guest debug exception 0xa0020007) that the base sample count (edi) is at least
/// 3, odd in mode 1 (esi) and even and at least 4 in mode 2. Game data now and then gives 2. The
/// count is made valid at the entry, the nearest larger valid one (deadinside28 raises it to 3 in
/// every mode, which mode 2 rejects too). Corrections are counted. BB_KERNEL_CLAMP=0: off.
/// The community's FPS patches (Kyo's "FPS++", all of them) turn the first check's jg into a jmp
/// (0xbbc40f): at high frame rates the count does come out below 3. The other four checks stay.
bool InstallKernelSampleClamp(u8* image) {
    if (const char* env = std::getenv("BB_KERNEL_CLAMP"); env && env[0] == '0') {
        return false;
    }
    u8* site = image + 0xbbc3e0;
    static constexpr u8 Entry[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}; // push rbp; mov rbp, rsp; push r15
    static constexpr u8 Check[4] = {0x41, 0x83, 0xfc, 0x02}; // +0x2b: cmp r12d, 2; then jg (or jmp)
    if (std::memcmp(site, Entry, sizeof(Entry)) != 0 ||
        std::memcmp(site + 0x2b, Check, sizeof(Check)) != 0 ||
        (site[0x2f] != 0x7f && site[0x2f] != 0xeb) || site[0x30] != 0x1d) {
        return false;
    }
    Emitter e;
    e.Bytes({0x50});                         // push rax (flags are free at a call)
    e.Bytes({0x89, 0xf8});                   // mov eax, edi (to tell a change)
    e.Bytes({0x83, 0xfe, 0x02});             // cmp esi, 2
    e.Bytes({0x74, 0x14});                   // je mode2
    e.Bytes({0x83, 0xff, 0x03});             // cmp edi, 3
    e.Bytes({0x7d, 0x05});                   // jge odd
    e.Bytes({0xbf, 0x03, 0x00, 0x00, 0x00}); // mov edi, 3
    e.Bytes({0x83, 0xfe, 0x01});             // odd: cmp esi, 1
    e.Bytes({0x75, 0x15});                   // jne done
    e.Bytes({0x83, 0xcf, 0x01});             // or edi, 1
    e.Bytes({0xeb, 0x10});                   // jmp done
    e.Bytes({0x83, 0xff, 0x04});             // mode2: cmp edi, 4
    e.Bytes({0x7d, 0x05});                   // jge even
    e.Bytes({0xbf, 0x04, 0x00, 0x00, 0x00}); // mov edi, 4
    e.Bytes({0x83, 0xc7, 0x01});             // even: add edi, 1
    e.Bytes({0x83, 0xe7, 0xfe});             // and edi, -2 (an odd count rounded up)
    e.Bytes({0x39, 0xf8});                   // done: cmp eax, edi
    e.Bytes({0x74, 0x0d});                   // je same
    e.Bytes({0x48, 0xb8});                   // movabs rax, &kernel_samples_fixed
    e.U64(reinterpret_cast<u64>(&kernel_samples_fixed));
    e.Bytes({0xf0, 0xff, 0x00});             // lock inc dword [rax]
    e.Bytes({0x58});                         // same: pop rax
    for (const u8 b : Entry) {
        e.Bytes({b});                        // the instructions the jump covers
    }
    if (!WriteJump(site, 0xe9, PlaceStub(e, reinterpret_cast<u64>(site) + sizeof(Entry)))) {
        return false;
    }
    site[5] = 0xcc; // never reached: the stub jumps back past it
    return true;
}

/// The arena allocation methods' entry (push rbp; mov rbp, rsp; push r15): ArenaAllocated(this,
/// size, caller) first.
bool HookArenaAlloc(u8* image, u64 offset) {
    u8* site = image + offset;
    static constexpr u8 Entry[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    if (std::memcmp(site, Entry, sizeof(Entry)) != 0) {
        return false;
    }
    Emitter e;
    e.SaveVolatile();
    // Saved below r12's own slot: r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax; then the return address.
    e.Bytes({0x49, 0x8b, 0x7c, 0x24, 0x28}); // mov rdi, [r12 + 0x28] (this)
    e.Bytes({0x49, 0x8b, 0x74, 0x24, 0x30}); // mov rsi, [r12 + 0x30] (size)
    e.Bytes({0x49, 0x8b, 0x54, 0x24, 0x50}); // mov rdx, [r12 + 0x50] (the caller)
    // Called from 0xe45560: its frame (rbp) and request (r14) are still live (read only then).
    e.Bytes({0x48, 0x89, 0xe9});             // mov rcx, rbp
    e.Bytes({0x4d, 0x89, 0xf0});             // mov r8, r14 (its request)
    e.CallAbs(reinterpret_cast<const void*>(&ArenaAllocated));
    e.RestoreVolatile();
    for (const u8 b : Entry) {
        e.Bytes({b});
    }
    if (!WriteJump(site, 0xe9, PlaceStub(e, reinterpret_cast<u64>(site) + sizeof(Entry)))) {
        return false;
    }
    site[5] = 0xcc;
    return true;
}

/// The render job pool: six slots, taken at 0x1056920 (sleeping until one is free) and given back
/// at 0x1056990, both only by the job runner's step at 0x1067f60: take a slot, check the job's
/// resource, run it, give the slot back. Its two error exits (the resource is not in the state
/// asked for; the run fails) leave through 0x1068092 returning 1 with the slot still held in
/// [runner + 0x120]: each failed job lost a slot for good, and after six every thread that needs
/// one slept for ever (the main thread and Havok's workers in Laurence's cinematic above 30 FPS).
/// The exits, and a runner reset with a job in the middle, now give the slot back.
/// BB_JOB_SLOT_FIX=0: as the game has it.
constexpr u64 JobGiveBack = 0x1056990;
constexpr u64 JobPool = 0x54b8788; ///< the pool 0x1057350 returns
std::atomic<u64> job_slots_released{0};

void NoteSlotReleased(const char* how) {
    const u64 n = job_slots_released.fetch_add(1) + 1;
    if (n <= 5 || (n & (n - 1)) == 0) {
        std::printf("Guest hooks: a render job %s gave its slot back (%llu so far)\n", how,
                    static_cast<unsigned long long>(n));
    }
}

GUEST_ABI void JobSlotFix(u64 runner, u64 pool, std::uint32_t result) {
    u64& held = *reinterpret_cast<u64*>(runner + 0x120);
    if (result != 1 || !held) {
        return;
    }
    using GiveBack = GUEST_ABI void (*)(u64 pool, u64 item);
    reinterpret_cast<GiveBack>(reinterpret_cast<u64>(guest_image) + JobGiveBack)(pool, held);
    held = 0;
    NoteSlotReleased("that failed");
}

GUEST_ABI void JobSlotDrop(u64 runner) {
    u64& held = *reinterpret_cast<u64*>(runner + 0x120);
    const u64 pool = *reinterpret_cast<const u64*>(guest_image + JobPool);
    if (!held || !pool) {
        return;
    }
    using GiveBack = GUEST_ABI void (*)(u64 pool, u64 item);
    reinterpret_cast<GiveBack>(reinterpret_cast<u64>(guest_image) + JobGiveBack)(pool, held);
    held = 0;
    NoteSlotReleased("reset in the middle");
}

/// A stub at `offset` over the `code` there: the handler with the saved registers, then that code.
/// `args` loads the handler's arguments (after SaveVolatile: the game's r12 is at [r12]);
/// `returns_rax`: the handler's result replaces the game's rax.
template <std::size_t N>
bool HookWithHandler(u8* image, u64 offset, const std::array<u8, N>& code,
                     std::initializer_list<u8> args, const void* handler,
                     bool returns_rax = false) {
    u8* site = image + offset;
    if (std::memcmp(site, code.data(), N) != 0) {
        return false;
    }
    Emitter e;
    e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); // lea rsp, [rsp - 128] (red zone)
    e.SaveVolatile();
    e.Bytes(args);
    e.CallAbs(handler);
    if (returns_rax) {
        e.Bytes({0x49, 0x89, 0x44, 0x24, 0x48}); // mov [r12 + 0x48], rax (the saved rax)
    }
    e.RestoreVolatile();
    e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
    for (const u8 b : code) {
        e.Bytes({b});
    }
    if (!WriteJump(site, 0xe9, PlaceStub(e, reinterpret_cast<u64>(site) + N))) {
        return false;
    }
    for (std::size_t i = 5; i < N; ++i) {
        site[i] = 0xcc; // never reached: the stub jumps back past them
    }
    return true;
}

bool InstallJobSlotFix(u8* image) {
    if (const char* env = std::getenv("BB_JOB_SLOT_FIX"); env && env[0] == '0') {
        return false;
    }
    // mov rax, [rbx]; cmp rax, [rbp - 0x30]: the stack check every exit of the step ends in.
    constexpr std::array<u8, 7> exit_code{0x48, 0x8b, 0x03, 0x48, 0x3b, 0x45, 0xd0};
    // mov qword [rbx + 0x120], 0: the runner reset.
    constexpr std::array<u8, 11> reset_code{0x48, 0xc7, 0x83, 0x20, 0x01, 0x00,
                                            0x00, 0x00, 0x00, 0x00, 0x00};
    const bool exit_in = HookWithHandler(image, 0x1068092, exit_code,
                                         {0x4c, 0x89, 0xf7,        // mov rdi, r14 (the runner)
                                          0x49, 0x8b, 0x34, 0x24,  // mov rsi, [r12] (the pool)
                                          0x44, 0x89, 0xfa},       // mov edx, r15d (the result)
                                         reinterpret_cast<const void*>(&JobSlotFix));
    const bool reset_in =
        HookWithHandler(image, 0x106798a, reset_code, {0x48, 0x89, 0xdf}, // mov rdi, rbx
                        reinterpret_cast<const void*>(&JobSlotDrop)) &&
        HookWithHandler(image, 0x1067a5c, reset_code, {0x48, 0x89, 0xdf},
                        reinterpret_cast<const void*>(&JobSlotDrop));
    return exit_in && reset_in;
}

/// The render resource cache (its range heap of 160 MiB, allocated at 0x216b21a): at high frame
/// rates it filled before Laurence's cinematic had its geometry, and the failed allocations made
/// the jobs above fail. It gets 512 MiB of the port's direct memory past the game's instead
/// (BB_RENDER_RESOURCE_CACHE_MB, 160..512; 160: the game's); the game's 160 MiB stays allocated,
/// so the memory areas load into keep their size. Returns the backing to use (rax).
GUEST_ABI u64 ResourceCacheBacking(u64 creation, u64 allocated) {
    constexpr std::uint32_t native_bytes = 160u << 20;
    std::uint32_t asked = 0;
    std::memcpy(&asked, reinterpret_cast<const void*>(creation + 0x54), sizeof(asked));
    static const std::uint32_t bytes = [] {
        const char* env = std::getenv("BB_RENDER_RESOURCE_CACHE_MB");
        const long mb = env ? std::strtol(env, nullptr, 10) : 512;
        return std::uint32_t(std::clamp(mb, 160l, 512l)) << 20;
    }();
    if (asked != native_bytes || !allocated || bytes == native_bytes) {
        return allocated;
    }
    void* block = nullptr;
    if (const int error = runtime_extra_direct_map(bytes, 64ull << 10, &block); error || !block) {
        std::printf("Render resource cache: no port memory for %u MiB (error %#x): the game's "
                    "160 MiB\n", bytes >> 20, unsigned(error));
        return allocated;
    }
    std::memcpy(reinterpret_cast<void*>(creation + 0x54), &bytes, sizeof(bytes));
    std::printf("Render resource cache: %u MiB in the port's memory (the game's 160 MiB stays "
                "allocated)\n", bytes >> 20);
    return reinterpret_cast<u64>(block);
}

bool InstallResourceCacheSize(u8* image) {
    // mov rcx, rbx; mov [rcx + 0x10], rax: right after the allocation returns (r14: the record).
    constexpr std::array<u8, 7> code{0x48, 0x89, 0xd9, 0x48, 0x89, 0x41, 0x10};
    return HookWithHandler(image, 0x216b21d, code,
                           {0x4c, 0x89, 0xf7,  // mov rdi, r14
                            0x48, 0x89, 0xc6}, // mov rsi, rax
                           reinterpret_cast<const void*>(&ResourceCacheBacking), true);
}

bool Wanted() {
    // Only with the game's memory in place (the launcher sets BB_GUEST_IN_PLACE from the memory
    // model before the game starts); BB_GUEST_HOOKS=0 leaves the game's code alone.
    const char* in_place = std::getenv("BB_GUEST_IN_PLACE");
    const char* hooks = std::getenv("BB_GUEST_HOOKS");
    return in_place && in_place[0] == '1' && !(hooks && hooks[0] == '0');
}
} // namespace

void PatchImage(unsigned char* image, std::uint64_t) {
    const bool memory_hooks = Wanted();
    const char* gx_env = std::getenv("BB_GX_NODE_GUARD");
    const bool gx_guard = !(gx_env && gx_env[0] == '0');
    const char* arena_env = std::getenv("BB_FRAME_ARENA_KB");
    const bool arena_wanted = !(arena_env && arena_env[0] == '0');
    // The blur kernel's sample counts are always made valid (BB_KERNEL_CLAMP=0 inside: off).
    auto* p = static_cast<u8*>(runtime_low_map(StubPageSize, 3));
    const u64 base = reinterpret_cast<u64>(image), at = reinterpret_cast<u64>(p);
    if (!p || (at > base ? at - base : base - at) >= (2000ull << 20)) {
        std::printf("Guest hooks: no memory near the image, not installed\n");
        return;
    }
    stub_page = p;
    int copies = 0, heap = 0;
    bool allocator = false;
    if (memory_hooks) {
        for (const auto& call : MemcpySites) {
            copies += HookCopyCall(image, call);
        }
        allocator = HookAllocReturn(image);
        const char* heap_env = std::getenv("BB_HEAP_HOOKS");
        if (!heap_env || heap_env[0] != '0') {
            for (const u64 site : HeapAllocReturns) {
                heap += HookHeapReturn(image, site);
            }
        }
    }
    const bool gx_in = gx_guard && GuardGxNode(image);
    const bool frame_in = gx_guard && GuardFrameNode(image);
    guest_image = image;
    const bool arena_in = arena_wanted && PatchFrameArena(image);
    const bool list_in = arena_wanted && HookDrawListSize(image);
    const bool clamp_in = InstallKernelSampleClamp(image);
    const bool slots_in = InstallJobSlotFix(image);
    const bool cache_in = InstallResourceCacheSize(image);
    if (const char* env = std::getenv("BB_FRAME_ARENA_TRACE"); env && env[0] == '1' && list_in) {
        arena_trace = HookArenaAlloc(image, 0xe45350) && HookArenaAlloc(image, 0xe453b0);
        std::printf("Guest hooks: frame arena trace %s\n", arena_trace ? "on" : "NOT installed");
    }
    DWORD old = 0;
    VirtualProtect(stub_page, StubPageSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub_page, StubPageSize);
    if (memory_hooks) {
        std::printf("Guest hooks: %d of %zu resource copy sites tell the GPU side of their writes; "
                    "GPU heap allocations seen at %d of %zu call sites; GPU memory allocator %s\n",
                    copies, MemcpySites.size(), heap, HeapAllocReturns.size(),
                    allocator ? "hooked" : "not found");
    }
    if (gx_guard) {
        std::printf("Guest hooks: GX command node guard %s, frame render command guard %s "
                    "(BB_GX_NODE_GUARD=0: off)\n",
                    gx_in ? "in" : "not installed (other code there)",
                    frame_in ? "in" : "not installed (other code there)");
    }
    if (arena_wanted) {
        std::printf("Guest hooks: frame render memory %s, draw list size %s (BB_FRAME_ARENA_KB)\n",
                    arena_in ? "made bigger" : "left as it is (other code there or 256)",
                    list_in ? "watched" : "not watched (other code there)");
    }
    std::printf("Guest hooks: blur kernel sample counts %s (BB_KERNEL_CLAMP=0: off)\n",
                clamp_in ? "made valid" : "left as they are (off or other code there)");
    std::printf("Guest hooks: failed render jobs %s, render resource cache %s\n",
                slots_in ? "give their slot back" : "keep their slot (off or other code there)",
                cache_in ? "watched" : "as the game has it (other code there)");
}

void Install(RangeCallback on_gpu_range_allocated) {
    on_range_allocated.store(on_gpu_range_allocated, std::memory_order_release);
}
} // namespace BbGuestHooks
#endif
