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
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void runtime_memory_note_write(uintptr_t address, uint64_t size);
extern "C" void* runtime_low_map(size_t size, int prot);

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
/// made bigger (BB_FRAME_ARENA_KB, default 512; 256: the game's own size).
constexpr std::array<u64, 2> FrameArenaSizeSites = {0x2195c09, 0x2195c46}; // mov edx, 0x40000
constexpr u64 FrameArenaGlobal = 0x55406c8;
constexpr u64 DrawListSizeSite = 0x2285d3b; // mov r12d, [r15 + 0x80]: the draw list's size
std::uint32_t frame_arena_bytes = 0x40000;
const u8* guest_image = nullptr;

struct ArenaUse {
    u64 used = 0, size = 0;
};
/// The current frame's arena (its linear allocator at +8: begin, end, current), or zeros.
ArenaUse CurrentFrameArena() {
    const u64 owner = *reinterpret_cast<const u64*>(guest_image + FrameArenaGlobal);
    if (owner == 0) {
        return {};
    }
    const int index = *reinterpret_cast<const int*>(owner + 0x380);
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
GUEST_ABI void DrawListSized(std::uint32_t list_bytes) {
    static std::atomic<bool> checked{false};
    const ArenaUse arena = CurrentFrameArena();
    if (!checked.exchange(true)) {
        std::printf("Guest hooks: frame render memory %llu KiB per frame (asked for %u KiB)%s\n",
                    (unsigned long long)(arena.size >> 10), frame_arena_bytes >> 10,
                    arena.size >= frame_arena_bytes ? "" : ": NOT GIVEN by the game's heap");
    }
    std::uint32_t seen = draw_list_peak.load(std::memory_order_relaxed);
    while (list_bytes > seen && !draw_list_peak.compare_exchange_weak(seen, list_bytes)) {
    }
    const auto used = std::uint32_t(arena.used);
    seen = arena_peak.load(std::memory_order_relaxed);
    while (used > seen && !arena_peak.compare_exchange_weak(seen, used)) {
    }
    const long long now = GetTickCount64();
    long long last = draw_list_report_ms.load(std::memory_order_relaxed);
    if (last == 0) {
        draw_list_report_ms.compare_exchange_strong(last, now);
    } else if (now - last >= 60000 && draw_list_report_ms.compare_exchange_strong(last, now)) {
        std::printf("Guest hooks: frame render memory: draw list up to %u KiB, %u of %llu KiB in "
                    "use when it was copied (largest of the minute)\n",
                    draw_list_peak.exchange(0) >> 10, arena_peak.exchange(0) >> 10,
                    (unsigned long long)(arena.size >> 10));
    }
}

bool PatchFrameArena(u8* image) {
    const char* env = std::getenv("BB_FRAME_ARENA_KB");
    const long kb = env && env[0] ? std::strtol(env, nullptr, 10) : 512;
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
    e.Bytes({0x44, 0x89, 0xe7});                                     // mov edi, r12d
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
    if (!memory_hooks && !gx_guard && !arena_wanted) {
        return;
    }
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
}

void Install(RangeCallback on_gpu_range_allocated) {
    on_range_allocated.store(on_gpu_range_allocated, std::memory_order_release);
}
} // namespace BbGuestHooks
#endif
