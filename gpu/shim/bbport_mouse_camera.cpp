// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the mouse turns the game's camera directly (Bloodborne 1.09), as in PC games.
//
// The camera object's update turns the camera from the right stick with an acceleration ramp and
// a spring. All its paths for a free camera meet at image+0x143ce67, after the frame's angles are
// written to the object (pitch +0x140, yaw +0x144, radians) and before the camera is built from
// them. A jmp there runs Stub (below): when the mouse moved, Frame adds the mouse turn to both
// pitches (inside the game's own limits) and to the yaw, and turns the drawn position (+0x100)
// around the pivot (+0xd0) by the same angles, so the spring has nothing to catch up with and a
// turn is drawn complete in the next frame. Stick, lock-on, distance and collisions stay the
// game's. The site and the camera layout were found by the bbport Windows forks (keyboard and
// mouse fork, runtime_camhook.c); this stub saves the whole AVX state around the call.
#include "bbport_mouse_camera.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
extern "C" void* runtime_low_map(size_t size, int prot);
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace BbMouseCamera {
namespace {

using u8 = std::uint8_t;
using u64 = std::uint64_t;

constexpr u64 HookSite = 0x143ce67;
// mov eax, [rbp-0x3c4]; test al, al; je +0x26
constexpr u8 SiteBytes[10] = {0x8b, 0x85, 0x3c, 0xfc, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x26};
// "Disable Camera Auto Rotation via Movement" (Imedved and Kyo, patches/Bloodborne.xml): the
// yaw writes of the camera's automatic turning while the character moves. With the mouse the
// camera turns only when the player turns it.
struct AutoRotation {
    u64 at;
    u8 original[9];
};
constexpr AutoRotation AutoRotationSites[] = {
    {0x143c6e8, {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x44, 0x01, 0x00, 0x00}},
    {0x143c984, {0xc4, 0xc1, 0x7a, 0x11, 0x8d, 0x30, 0x01, 0x00, 0x00}},
    {0x143dde6, {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x94, 0x02, 0x00, 0x00}},
    {0x143c870, {0xc4, 0xc1, 0x7a, 0x11, 0xad, 0x44, 0x01, 0x00, 0x00}},
};

// Camera object fields (floats).
enum : unsigned {
    CamPitch = 0x140, CamYaw = 0x144, CamTargetPitch = 0x150, CamPitchMax = 0x1ec,
    CamPitchMin = 0x1f0, CamPivot = 0xd0, CamDrawn = 0x100
};

std::atomic<bool> installed{false};

float* Field(u8* camera, unsigned offset) {
    return reinterpret_cast<float*>(camera + offset);
}

} // namespace
} // namespace BbMouseCamera

// The stub's data: the pending turn (pitch, yaw: two floats in one word, taken atomically), where
// the game continues (HookSite + 6) and Frame.
// Referenced from the stub's assembly only: kept through LTO (used).
extern "C" {
__attribute__((used)) alignas(8) std::uint64_t bb_mouse_camera_delta = 0;
__attribute__((used)) std::uint64_t bb_mouse_camera_back = 0;
float __attribute__((sysv_abi)) bb_mouse_camera_frame(std::uint8_t* camera, float pitch_in);
}

// On the game's camera thread, from the stub (System V ABI, like the game). Returns the pitch the
// game goes on with (its xmm0).
extern "C" __attribute__((used)) float __attribute__((sysv_abi))
bb_mouse_camera_frame(std::uint8_t* camera, float pitch_in) {
    using namespace BbMouseCamera;
    const std::uint64_t taken = __atomic_exchange_n(&bb_mouse_camera_delta, 0, __ATOMIC_ACQ_REL);
    if (!taken) {
        return pitch_in;
    }
    float turn[2];
    std::memcpy(turn, &taken, 8);
    const float pitch_old = *Field(camera, CamPitch), yaw_old = *Field(camera, CamYaw);
    float pitch = pitch_old + turn[0];
    pitch = std::fmax(pitch, *Field(camera, CamPitchMin));
    pitch = std::fmin(pitch, *Field(camera, CamPitchMax));
    *Field(camera, CamPitch) = pitch;
    *Field(camera, CamTargetPitch) = pitch;
    *Field(camera, CamYaw) = yaw_old + turn[1];
    // The drawn position sits at pivot - direction * r, direction = (sin y cos p, -sin p,
    // cos y cos p): turned by the same yaw and pitch.
    const float* pivot = Field(camera, CamPivot);
    float* drawn = Field(camera, CamDrawn);
    const double ox = drawn[0] - pivot[0], oy = drawn[1] - pivot[1], oz = drawn[2] - pivot[2];
    const double r = std::sqrt(ox * ox + oy * oy + oz * oz);
    if (r > 1e-3 && r < 100) {
        const double s = std::fmax(-1.0, std::fmin(1.0, oy / r));
        const double p = std::asin(s) + (pitch - pitch_old), y = std::atan2(-ox, -oz) + turn[1];
        drawn[0] = float(pivot[0] - std::sin(y) * std::cos(p) * r);
        drawn[1] = float(pivot[1] + std::sin(p) * r);
        drawn[2] = float(pivot[2] - std::cos(y) * std::cos(p) * r);
    }
    return pitch;
}

// Jumped to from HookSite (through the near page). Without a pending turn: the replaced
// instruction and back. Otherwise every register the game may hold is saved (the System V
// caller-saved ones, all 16 ymm registers whole), Frame runs with the camera object (r13) and the
// frame's pitch (xmm0, lane 0) and its result replaces that lane.
extern "C" __attribute__((naked, used)) void bb_mouse_camera_stub() {
    __asm__ volatile(
        "cmpq $0, bb_mouse_camera_delta(%rip)\n\t"
        "je 1f\n\t"
        "pushq %rbx\n\t"
        "movq %rsp, %rbx\n\t"
        "andq $-32, %rsp\n\t"
        "subq $0x260, %rsp\n\t"
        "vmovups %ymm0, 0x000(%rsp)\n\t"
        "vmovups %ymm1, 0x020(%rsp)\n\t"
        "vmovups %ymm2, 0x040(%rsp)\n\t"
        "vmovups %ymm3, 0x060(%rsp)\n\t"
        "vmovups %ymm4, 0x080(%rsp)\n\t"
        "vmovups %ymm5, 0x0a0(%rsp)\n\t"
        "vmovups %ymm6, 0x0c0(%rsp)\n\t"
        "vmovups %ymm7, 0x0e0(%rsp)\n\t"
        "vmovups %ymm8, 0x100(%rsp)\n\t"
        "vmovups %ymm9, 0x120(%rsp)\n\t"
        "vmovups %ymm10, 0x140(%rsp)\n\t"
        "vmovups %ymm11, 0x160(%rsp)\n\t"
        "vmovups %ymm12, 0x180(%rsp)\n\t"
        "vmovups %ymm13, 0x1a0(%rsp)\n\t"
        "vmovups %ymm14, 0x1c0(%rsp)\n\t"
        "vmovups %ymm15, 0x1e0(%rsp)\n\t"
        "movq %rax, 0x200(%rsp)\n\t"
        "movq %rcx, 0x208(%rsp)\n\t"
        "movq %rdx, 0x210(%rsp)\n\t"
        "movq %rsi, 0x218(%rsp)\n\t"
        "movq %rdi, 0x220(%rsp)\n\t"
        "movq %r8, 0x228(%rsp)\n\t"
        "movq %r9, 0x230(%rsp)\n\t"
        "movq %r10, 0x238(%rsp)\n\t"
        "movq %r11, 0x240(%rsp)\n\t"
        "movq %r13, %rdi\n\t"
        "callq bb_mouse_camera_frame\n\t"
        "vmovss %xmm0, 0x000(%rsp)\n\t"
        "vmovups 0x000(%rsp), %ymm0\n\t"
        "vmovups 0x020(%rsp), %ymm1\n\t"
        "vmovups 0x040(%rsp), %ymm2\n\t"
        "vmovups 0x060(%rsp), %ymm3\n\t"
        "vmovups 0x080(%rsp), %ymm4\n\t"
        "vmovups 0x0a0(%rsp), %ymm5\n\t"
        "vmovups 0x0c0(%rsp), %ymm6\n\t"
        "vmovups 0x0e0(%rsp), %ymm7\n\t"
        "vmovups 0x100(%rsp), %ymm8\n\t"
        "vmovups 0x120(%rsp), %ymm9\n\t"
        "vmovups 0x140(%rsp), %ymm10\n\t"
        "vmovups 0x160(%rsp), %ymm11\n\t"
        "vmovups 0x180(%rsp), %ymm12\n\t"
        "vmovups 0x1a0(%rsp), %ymm13\n\t"
        "vmovups 0x1c0(%rsp), %ymm14\n\t"
        "vmovups 0x1e0(%rsp), %ymm15\n\t"
        "movq 0x200(%rsp), %rax\n\t"
        "movq 0x208(%rsp), %rcx\n\t"
        "movq 0x210(%rsp), %rdx\n\t"
        "movq 0x218(%rsp), %rsi\n\t"
        "movq 0x220(%rsp), %rdi\n\t"
        "movq 0x228(%rsp), %r8\n\t"
        "movq 0x230(%rsp), %r9\n\t"
        "movq 0x238(%rsp), %r10\n\t"
        "movq 0x240(%rsp), %r11\n\t"
        "movq %rbx, %rsp\n\t"
        "popq %rbx\n"
        "1:\n\t"
        "movl -0x3c4(%rbp), %eax\n\t"
        "jmpq *bb_mouse_camera_back(%rip)\n\t");
}

namespace BbMouseCamera {
namespace {

/// A page within a rel32 jump of the image (the hook site jumps to it).
u8* MapNear(unsigned char* image, u64 image_size) {
#ifdef _WIN32
    (void)image_size;
    auto* p = static_cast<u8*>(runtime_low_map(4096, 3));
    const u64 base = reinterpret_cast<u64>(image), at = reinterpret_cast<u64>(p);
    const u64 distance = at > base ? at - base : base - at;
    return p && distance < (2000ull << 20) ? p : nullptr;
#else
    const u64 base = reinterpret_cast<u64>(image);
    const std::size_t page = std::size_t(sysconf(_SC_PAGESIZE));
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {base - k * (24ull << 20), base + image_size + k * (24ull << 20)}) {
            void* p = mmap(reinterpret_cast<void*>(hint), page, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                return static_cast<u8*>(p);
            }
        }
    }
    return nullptr;
#endif
}

void MakeExecutable(u8* page) {
#ifdef _WIN32
    DWORD old;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
#else
    mprotect(page, std::size_t(sysconf(_SC_PAGESIZE)), PROT_READ | PROT_EXEC);
#endif
}

} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    if (const char* env = std::getenv("BB_MOUSE_CAMERA"); env && env[0] == '0') {
        return;
    }
    if (size < HookSite + sizeof(SiteBytes) || std::memcmp(image + HookSite, SiteBytes, sizeof(SiteBytes))) {
        std::printf("Mouse camera: unknown game code at the camera update; the mouse turns the stick\n");
        return;
    }
    u8* page = MapNear(image, size);
    if (!page) {
        std::printf("Mouse camera: no memory near the image; the mouse turns the stick\n");
        return;
    }
    // jmp [rip+0] / absolute address of the stub (in the port's executable, anywhere).
    const u64 stub = reinterpret_cast<u64>(&bb_mouse_camera_stub);
    const u8 jump[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(page, jump, sizeof(jump));
    std::memcpy(page + sizeof(jump), &stub, 8);
    MakeExecutable(page);
    bb_mouse_camera_back = reinterpret_cast<u64>(image) + HookSite + 6;
    // The image is still writable here (bbgpu_patch_image runs before it is protected).
    const std::int64_t rel = std::int64_t(reinterpret_cast<u64>(page)) -
                             std::int64_t(reinterpret_cast<u64>(image) + HookSite + 5);
    const std::int32_t rel32 = std::int32_t(rel);
    u8 site[6] = {0xe9, 0, 0, 0, 0, 0x90};
    std::memcpy(site + 1, &rel32, 4);
    std::memcpy(image + HookSite, site, sizeof(site));
    int done = 0;
    for (const auto& s : AutoRotationSites) {
        u8* at = image + s.at;
        static const u8 nops[9] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        if (!std::memcmp(at, nops, 9)) {
            ++done;
        } else if (!std::memcmp(at, s.original, 9)) {
            std::memcpy(at, nops, 9);
            ++done;
        }
    }
    installed = true;
    std::printf("Mouse camera: hooked into the game's camera update (auto-rotation off at %d of 4 sites; "
                "BB_MOUSE_CAMERA=0 turns the hook off)\n", done);
}

bool Active() {
    return installed.load(std::memory_order_relaxed);
}

void Turn(float pitch, float yaw) {
    if (!Active() || (pitch == 0.0f && yaw == 0.0f)) {
        return;
    }
    std::uint64_t seen = __atomic_load_n(&bb_mouse_camera_delta, __ATOMIC_RELAXED), next;
    do {
        float pair[2];
        std::memcpy(pair, &seen, 8);
        pair[0] += pitch;
        pair[1] += yaw;
        std::memcpy(&next, pair, 8);
    } while (!__atomic_compare_exchange_n(&bb_mouse_camera_delta, &seen, next, false, __ATOMIC_ACQ_REL,
                                          __ATOMIC_RELAXED));
}

} // namespace BbMouseCamera
