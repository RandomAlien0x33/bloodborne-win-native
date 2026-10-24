// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the game's frame rate set free by hooks in its code (bbport_frame_rate.h).
//
// The game's main loop (0x2018d20) runs its frame pacer (0x2034770: it waits for the frame time
// its mode asks, 1/30 s, and measures the frame at [pacer + 0x264]) and then steps the game by a
// fixed 1/30 s (0x2018e36). The hooks keep the pacer's mode at one vblank, with the limit's frame
// time as the target, and step the game by the measured frame time (1/480 to 1/30 s: below 30 FPS
// the game slows down, as it always did).
//
// What the game ties to a 30 FPS frame besides: subsystems stepped by a fixed 1/30 s, per-frame
// steps and approaches, per-frame damping. Their instructions read the constant from a page next
// to the image instead (only the instruction's address operand changes), where FrameStep writes
// the value for this frame's time: a step of 1/30 s becomes the frame's time, a per-frame factor
// f becomes f^(frames) for the frame's length in 30 FPS frames.
#include "bbport_frame_rate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>

#include "bbport_settings.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

extern "C" void* runtime_low_map(size_t size, int prot);

namespace BbFrameRate {
namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;
#define GUEST_ABI __attribute__((sysv_abi))

std::atomic<bool> unlocked{false};
std::atomic<bool> limit30{false};
u64 image_base = 0;

constexpr float Frame30 = 1.0f / 30.0f;
/// The highest frame rate the pacer allows: faster, the game's main loop outran its worker threads
/// on loading screens (a pool allocator popped a freed node at 880-1000 FPS).
constexpr int MaxRate = 480;

/// What the redirected instructions read, rewritten by FrameStep for each frame.
struct alignas(64) Live {
    float dt = Frame30;            ///< the frame's time: the game steps by it
    float shortest = Frame30;      ///< the pacer's frame time target (the limit)
    float wind_dt = Frame30;       ///< the frame's time smoothed (foliage wind)
    float pad = 0.0f;
    float decay95[4] = {0.95f, 0.95f, 0.95f, 0.95f}; ///< 0.95 per 30 FPS frame (read as four)
    float approach30 = Frame30;    ///< a 1/30 approach per 30 FPS frame
    float approach10 = 0.1f;       ///< a 0.1 approach per 30 FPS frame
    float step_down = -0.1f;       ///< -0.1 per 30 FPS frame
    float step_up = 0.1f;          ///< +0.1 per 30 FPS frame
    float decay08 = 0.8f;          ///< 0.8 per 30 FPS frame
    float cloth = 1.0f;            ///< cloth constraints' stiffness
};
Live* live = nullptr;

/// The menus' limit (or F3's 30) as the pacer's frame time.
float Shortest() {
    if (limit30.load(std::memory_order_relaxed)) {
        return Frame30;
    }
    const int index = std::clamp(BbSettings::Get().frame_rate.load(std::memory_order_relaxed), 0,
                                 BbSettings::FrameRateCount - 1);
    const int rate = BbSettings::FrameRates[index].rate;
    return 1.0f / float(std::clamp(rate > 0 ? rate : MaxRate, 30, MaxRate));
}

/// The pacer at the main loop's step (rdi): this frame's values.
GUEST_ABI void FrameStep(u64 pacer) {
    const float shortest = Shortest();
    live->shortest = shortest;
    const float measured = *reinterpret_cast<const float*>(pacer + 0x264);
    const float dt = std::clamp(std::isfinite(measured) ? measured : Frame30, shortest, Frame30);
    const float frames = dt * 30.0f; // the frame's length in 30 FPS frames
    live->dt = dt;
    // Foliage wind twitched with frame-to-frame jitter of the measurement: it follows the frame
    // time by 0.15 a frame (time stays right on average).
    live->wind_dt += (dt - live->wind_dt) * 0.15f;
    const float decay95 = std::pow(0.95f, frames);
    for (float& d : live->decay95) {
        d = decay95;
    }
    live->approach30 = 1.0f - std::pow(1.0f - Frame30, frames);
    live->approach10 = 1.0f - std::pow(0.9f, frames);
    live->step_down = -0.1f * frames;
    live->step_up = 0.1f * frames;
    live->decay08 = std::pow(0.8f, frames);
    // Cloth is stiffer the more often it is solved: the game's 1.0 at 30 FPS, 0.5 from 60 FPS up
    // (what the community's 60 FPS patches settled on), the bound its setups clamp to.
    live->cloth = std::clamp(frames, 0.5f, 1.0f);
}

// ---- Redirected constants: instruction (its RIP-relative operand), the constant, the value ----
struct Redirect {
    u64 site;
    u64 constant;
    std::size_t value; ///< offsetof(Live, ..)
};
#define LIVE(field) offsetof(Live, field)
constexpr Redirect Redirects[] = {
    // A fixed 1/30 s step or "one frame":
    {0xab9f0d, 0x47efdcc, LIVE(dt)},   // a time step, one frame when unset
    {0xdca3ed, 0x4888688, LIVE(dt)},   // a per-frame budget
    {0xee596a, 0x48ad65c, LIVE(dt)},   // effects: a fade of at least one frame (three releases)
    {0xee5a8a, 0x48ad660, LIVE(dt)},
    {0xee5baa, 0x48ad664, LIVE(dt)},
    {0xef06f1, 0x48adc08, LIVE(dt)},   // at least one frame
    {0xef6992, 0x48add28, LIVE(dt)},
    {0x1044943, 0x48cc3f4, LIVE(dt)},  // an update by one frame
    {0x12a4416, 0x492577c, LIVE(dt)},  // a timer advanced by one frame each frame
    {0x15dc1f7, 0x4926c9c, LIVE(dt)},
    {0x173456e, 0x49270e4, LIVE(dt)},  // an update by one frame
    {0x17f9d26, 0x4927554, LIVE(dt)},
    {0x180f647, 0x49275f4, LIVE(dt)},  // a timer advanced by one frame each frame
    {0x18bc4f7, 0x4927c4c, LIVE(dt)},  // an update by one frame (twice)
    {0x18bc50d, 0x4927c4c, LIVE(dt)},
    {0x1dbc308, 0x4928e98, LIVE(dt)},  // frames counted at one frame's time
    {0x1f77d4a, 0x4929170, LIVE(dt)},  // an update by one frame (0 after a reset)
    {0x227784b, 0x4929b80, LIVE(dt)},  // timers advanced by one frame (0xebbda0)
    {0x2277892, 0x4929b80, LIVE(dt)},
    {0x2313d8d, 0x4929e70, LIVE(wind_dt)}, // foliage wind: 1/60 in the game's 60 Hz mode,
    {0x2313dad, 0x4929e70, LIVE(dt)},  // a simulation step: 1/60 in the 60 Hz mode,
    {0x2313db7, 0x4929e74, LIVE(dt)},  // 1/30 otherwise
    {0x2315d71, 0x4929e74, LIVE(dt)},
    // Body parts the physics moves (0x1515ec0): their timers advanced by 1/30 s.
    {0x1515fab, 0x4926364, LIVE(dt)},
    {0x1516030, 0x4926364, LIVE(dt)},
    // Steps and approaches per frame:
    {0x19084f5, 0x4927fd4, LIVE(step_down)}, // a value ramped by 0.1 a frame
    {0x1908522, 0x4927fd4, LIVE(step_down)},
    {0x1908518, 0x4927fe0, LIVE(step_up)},
    {0x190854c, 0x4927fe0, LIVE(step_up)},
    {0x143e2b2, 0x4925e94, LIVE(approach30)}, // the follow camera's approaches
    {0x143af8c, 0x4925e68, LIVE(approach10)},
    // Damping per frame:
    {0x1a4abca, 0x490b5b0, LIVE(decay95)}, // a character's motion (x0.95 a frame)
    {0x1514c4d, 0x4926334, LIVE(decay08)}, // the run's wall check (x0.8 a frame while blocked)
    // Cloth constraints' stiffness:
    {0x47c6dc, 0x47693f0, LIVE(cloth)},
    {0x7e45be, 0x47b63e4, LIVE(cloth)},
    {0x7e4642, 0x47b63e4, LIVE(cloth)},
    {0x7e6239, 0x47b63e4, LIVE(cloth)},
    {0x7f706d, 0x47b8474, LIVE(cloth)},
    {0x7f70ad, 0x47b8474, LIVE(cloth)},
    {0x7f79e5, 0x47b8474, LIVE(cloth)},
    {0x7f7be8, 0x47b8474, LIVE(cloth)},
    {0x7f7dbd, 0x47b8474, LIVE(cloth)},
    {0x805898, 0x47b91c0, LIVE(cloth)},
    {0x805d18, 0x47b91c0, LIVE(cloth)},
    {0x806269, 0x47b91c0, LIVE(cloth)},
    {0x8062f0, 0x47b91c0, LIVE(cloth)},
    {0x809d62, 0x47b98b8, LIVE(cloth)},
    {0x80a4c9, 0x47b98b8, LIVE(cloth)},
    {0x80a876, 0x47b98b8, LIVE(cloth)},
};
#undef LIVE

/// The instruction at `r.site` reads `r.constant` RIP-relative (a VEX instruction, no immediate
/// after the operand): it reads the live value instead.
bool RedirectOperand(const Redirect& r) {
    u8* code = reinterpret_cast<u8*>(image_base + r.site);
    std::size_t disp_at = 0, length = 0;
    if (code[0] == 0xc5 && (code[3] & 0xc7) == 0x05) {
        disp_at = 4, length = 8;
    } else if (code[0] == 0xc4 && (code[4] & 0xc7) == 0x05) {
        disp_at = 5, length = 9;
    } else {
        return false;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, code + disp_at, 4);
    const u64 next = image_base + r.site + length;
    if (next + std::int64_t(disp) != image_base + r.constant) {
        return false;
    }
    const std::int64_t to = std::int64_t(reinterpret_cast<u64>(live) + r.value) - std::int64_t(next);
    if (to != std::int32_t(to)) {
        return false;
    }
    const std::int32_t to32 = std::int32_t(to);
    std::memcpy(code + disp_at, &to32, 4);
    return true;
}

// ---- Detours: the instructions at a site replaced by a jump to a stub ----
struct Emitter {
    std::array<u8, 256> code{};
    std::size_t size = 0;
    bool overflow = false;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            if (size < code.size()) {
                code[size] = b;
            }
            ++size;
        }
        overflow = overflow || size > code.size();
    }
    void U64(u64 v) {
        for (int i = 0; i < 8; ++i) {
            Bytes({u8(v >> (8 * i))});
        }
    }
    /// Saves the registers the handler may change (the game's convention), aligns the stack.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12 (holds the unaligned stack pointer)
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0}); // call rax
    }
    /// jmp [rip]: to `target` anywhere.
    void JmpAbs(u64 target) {
        Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
        U64(target);
    }
    /// The 32-bit value at `address` into the stack frame's [rbp + offset] (rax kept).
    void StoreToFrame(const void* address, std::int8_t offset) {
        Bytes({0x50, 0x48, 0xb8}); // push rax; movabs rax, address
        U64(reinterpret_cast<u64>(address));
        Bytes({0x8b, 0x00});                   // mov eax, [rax]
        Bytes({0x89, 0x45, u8(offset), 0x58}); // mov [rbp + offset], eax; pop rax
    }
};

u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 4096;

/// `e` into the stub page; a jump to it at `site` over `length` bytes (the rest int3) if the site
/// holds `expected`.
bool Detour(u64 site, const u8* expected, std::size_t length, const Emitter& e) {
    u8* code = reinterpret_cast<u8*>(image_base + site);
    if (length < 5 || std::memcmp(code, expected, length) != 0 || e.overflow ||
        stub_used + e.size > StubPageSize) {
        return false;
    }
    u8* stub = stub_page + stub_used;
    const std::int64_t rel =
        std::int64_t(reinterpret_cast<u64>(stub)) - std::int64_t(image_base + site + 5);
    if (rel != std::int32_t(rel)) {
        return false;
    }
    std::memcpy(stub, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    const std::int32_t rel32 = std::int32_t(rel);
    code[0] = 0xe9;
    std::memcpy(code + 1, &rel32, 4);
    for (std::size_t i = 5; i < length; ++i) {
        code[i] = 0xcc;
    }
    return true;
}

/// The pacer's mode, after the game chose it (0x203493d, `lea r15, [r12 + 0x268]`; r12 the
/// pacer): one vblank (+0x10), no flip skipping (+0x14), the limit's frame time as the target
/// (+0x18), no vblank counts to wait for (+0x268, +0x26c).
bool HookPacerMode() {
    constexpr u8 code[] = {0x4d, 0x8d, 0xbc, 0x24, 0x68, 0x02, 0x00, 0x00};
    Emitter e;
    e.Bytes({0x41, 0xc7, 0x44, 0x24, 0x10, 0x01, 0x00, 0x00, 0x00}); // mov dword [r12 + 0x10], 1
    e.Bytes({0x41, 0xc6, 0x44, 0x24, 0x14, 0x00});                   // mov byte [r12 + 0x14], 0
    e.Bytes({0x50, 0x48, 0xb8});                                       // push rax; movabs rax,
    e.U64(reinterpret_cast<u64>(&live->shortest));                     //   &shortest
    e.Bytes({0x8b, 0x00});                                             // mov eax, [rax]
    e.Bytes({0x41, 0x89, 0x44, 0x24, 0x18, 0x58});                     // mov [r12 + 0x18], eax; pop rax
    e.Bytes({0x49, 0xc7, 0x84, 0x24, 0x68, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // mov qword [r12 + 0x268], 0
    for (const u8 b : code) {
        e.Bytes({b});
    }
    e.JmpAbs(image_base + 0x203493d + sizeof(code));
    return Detour(0x203493d, code, sizeof(code), e);
}

/// The pacer keeps its one vblank when a frame came late (0x2034d96: `test dl, dl; je; mov eax,
/// [r12 + 0x10]`, then eax is stored back: 0 when late).
bool HookPacerLate() {
    constexpr u8 code[] = {0x84, 0xd2, 0x74, 0x05, 0x41, 0x8b, 0x44, 0x24, 0x10};
    Emitter e;
    e.Bytes({0x84, 0xd2});                   // test dl, dl
    e.Bytes({0x41, 0x8b, 0x44, 0x24, 0x10}); // mov eax, [r12 + 0x10]
    e.JmpAbs(image_base + 0x2034d9f);
    return Detour(0x2034d96, code, sizeof(code), e);
}

/// The main loop's step (0x2018e36: `mov dword [rbp - 0x38], 1/30`, its time; rbx the pacer):
/// FrameStep, then the frame's time.
bool HookStep() {
    constexpr u8 code[] = {0xc7, 0x45, 0xc8, 0x89, 0x88, 0x08, 0x3d};
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xdf}); // mov rdi, rbx
    e.CallAbs(reinterpret_cast<const void*>(&FrameStep));
    e.RestoreVolatile();
    e.StoreToFrame(&live->dt, -0x38);
    e.JmpAbs(image_base + 0x2018e36 + sizeof(code));
    return Detour(0x2018e36, code, sizeof(code), e);
}

/// A subsystem stepped by a fixed 1/30 s (`mov dword [rbp + offset], 1/30` at `site`): by the
/// frame's time.
bool HookFixedStep(u64 site, std::int8_t offset) {
    const u8 code[] = {0xc7, 0x45, u8(offset), 0x89, 0x88, 0x08, 0x3d};
    Emitter e;
    e.StoreToFrame(&live->dt, offset);
    e.JmpAbs(image_base + site + sizeof(code));
    return Detour(site, code, sizeof(code), e);
}

/// The run's wall check (0x1514c24: the distance moved this frame x 30 = a speed at 30 FPS): the
/// distance divided by the frame's time ([rbp - 0x74]), a speed at any frame rate. Above ~90 FPS
/// the per-frame distance fell under the check's threshold and the run dropped to half speed.
/// 1: done by the hook, 2: a Sprint Fix patch already did it, 0: other code.
int HookRunSpeed() {
    constexpr u64 site = 0x1514c24;
    const u8* code = reinterpret_cast<const u8*>(image_base + site);
    constexpr u8 patched[] = {0xc5, 0xfa, 0x5e, 0x45, 0x8c, 0x0f, 0x1f, 0x00}; // vdivss; nop
    if (std::memcmp(code, patched, sizeof(patched)) == 0) {
        return 2;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, code + 4, 4);
    if (code[0] != 0xc5 || code[1] != 0xfa || code[2] != 0x59 || code[3] != 0x05 ||
        image_base + site + 8 + std::int64_t(disp) != image_base + 0x492632c) {
        return 0;
    }
    u8 expected[8];
    std::memcpy(expected, code, 8);
    Emitter e;
    e.Bytes({0xc5, 0xfa, 0x5e, 0x45, 0x8c}); // vdivss xmm0, xmm0, [rbp - 0x74]
    e.JmpAbs(image_base + site + 8);
    return Detour(site, expected, 8, e) ? 1 : 0;
}

/// A page within +-2 GiB of the image, for rel32 jumps and RIP-relative operands.
u8* MapNear() {
    auto* p = static_cast<u8*>(runtime_low_map(StubPageSize, 3));
    const u64 at = reinterpret_cast<u64>(p);
    if (!p || (at > image_base ? at - image_base : image_base - at) >= (2000ull << 20)) {
        return nullptr;
    }
    return p;
}

void ProtectStubs() {
#ifdef _WIN32
    DWORD old = 0;
    VirtualProtect(stub_page, StubPageSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub_page, StubPageSize);
#else
    mprotect(stub_page, StubPageSize, PROT_READ | PROT_EXEC);
#endif
}
} // namespace

void PatchImage(unsigned char* image, std::uint64_t) {
    if (const char* hooks = std::getenv("BB_FRAME_HOOKS"); hooks && hooks[0] == '0') {
        return;
    }
    image_base = reinterpret_cast<u64>(image);
    u8* data = MapNear();
    stub_page = MapNear();
    if (!data || !stub_page) {
        std::printf("Frame rate: no memory next to the game's code: 30 FPS\n");
        return;
    }
    live = new (data) Live{};
    // The core first: without all three the game keeps its own frame rate.
    if (!(HookPacerMode() && HookPacerLate() && HookStep())) {
        std::printf("Frame rate: the game's frame pacer is not the expected code: 30 FPS\n");
        ProtectStubs();
        return;
    }
    unlocked = true;
    const int steps = HookFixedStep(0x17f9b71, -0x48) + HookFixedStep(0x1572a7d, -0x30);
    const int run = HookRunSpeed();
    int redirected = 0;
    for (const Redirect& r : Redirects) {
        if (RedirectOperand(r)) {
            ++redirected;
        } else {
            std::printf("Frame rate: %#llx is not the expected code, left as it is\n",
                        (unsigned long long)r.site);
        }
    }
    ProtectStubs();
    std::printf("Frame rate: by hooks (no frame rate patches): the game steps by each frame's time "
                "(up to 1/30 s); %d fixed steps, %d of %zu per-frame values follow the frame's "
                "time, the run's wall check %s\n",
                steps, redirected, std::size(Redirects),
                run == 1 ? "by speed" : run == 2 ? "by speed (a Sprint Fix patch did it already)"
                                                 : "unchanged (not found)");
}

bool Unlocked() {
    return unlocked.load(std::memory_order_relaxed);
}

void ToggleLimit30() {
    const bool on = !limit30.load(std::memory_order_relaxed);
    limit30.store(on, std::memory_order_relaxed);
    std::printf("Frame rate: F3, 30 FPS %s\n", on ? "on" : "off");
}
} // namespace BbFrameRate
