// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_photo.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace BbPhoto {
namespace {

// The game calls and is called with the System V ABI, not the host's Win64 one.
#ifdef _WIN32
#define GUEST_ABI __attribute__((sysv_abi))
#else
#define GUEST_ABI
#endif

using u64 = std::uint64_t;
using u32 = std::uint32_t;

/// FindCharacter (entity, 0, out): the character of an entity id; the player is 10000.
constexpr u64 FindCharacter = 0x13c97a0;
constexpr u32 PlayerEntity = 10000;
/// The scripts' SetDrawEnable (Lua 0x132dec0: this, entity, on): the character's model drawn or not.
constexpr u64 SetDrawEnable = 0x132dec0;
/// A gauge's wrapper of a bar clip (vtable) and Scaleform's AS3 object interface.
constexpr u64 GaugeWrapperVtable = 0x5399f90, As3ObjectInterface = 0x51b9570;
/// Kept as asked: the game shows the HUD again after menus and loads, a load draws the model.
constexpr auto Refresh = std::chrono::milliseconds(250);

unsigned char* image = nullptr;
std::mutex gauge_lock;
std::vector<u64> gauges; ///< under gauge_lock

template <typename F>
F Game(u64 va) {
    return reinterpret_cast<F>(reinterpret_cast<u64>(image) + va);
}

bool SafeRead(u64 at, void* out, std::size_t size) {
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, size, &got) &&
           got == size;
#else
    std::memcpy(out, reinterpret_cast<const void*>(at), size);
    return true;
#endif
}

/// A display object: its Scaleform object interface and data.
struct Clip {
    u64 oi = 0, data = 0;
};

/// The gauge's fill clip (its wrapper at +0: the value's interface +0x38, data +0x48).
Clip FillOf(u64 gauge) {
    Clip c;
    u64 vt = 0;
    if (!SafeRead(gauge, &vt, 8) || vt != reinterpret_cast<u64>(image) + GaugeWrapperVtable ||
        !SafeRead(gauge + 0x38, &c.oi, 8) || !SafeRead(gauge + 0x48, &c.data, 8) || !c.oi ||
        !SafeRead(c.oi, &vt, 8) || vt != reinterpret_cast<u64>(image) + As3ObjectInterface) {
        return {};
    }
    return c;
}

/// A Scaleform value as this build lays it out (GFx::Value with its managed-list links).
struct GfxValue {
    u64 prev = ~0ull, next = ~0ull;
    u64 oi = 0;
    u32 type = 0, pad = 0;
    u64 data = 0, aux = 0;
};
constexpr u32 ValueBool = 2, ValueString = 6, ValueDisplayObject = 10, ValueManaged = 0x40;

/// GetMember (interface +0x28: pdata, name, result, is a display object); the result is released
/// by the caller (ReleaseValue).
bool Member(const Clip& c, const char* name, GfxValue& out) {
    out = GfxValue{};
    using F = bool(GUEST_ABI*)(u64, u64, const char*, GfxValue*, bool);
    const u64 vt = *reinterpret_cast<u64*>(c.oi);
    return reinterpret_cast<F>(*reinterpret_cast<u64*>(vt + 0x28))(c.oi, c.data, name, &out, true);
}

/// ObjectRelease (interface +0x18: value, its data).
void ReleaseValue(GfxValue& v) {
    if ((v.type & ValueManaged) && v.oi) {
        using F = void(GUEST_ABI*)(u64, GfxValue*, u64);
        const u64 vt = *reinterpret_cast<u64*>(v.oi);
        reinterpret_cast<F>(*reinterpret_cast<u64*>(vt + 0x18))(v.oi, &v, v.data);
    }
    v = GfxValue{};
}

std::string Name(const Clip& c) {
    GfxValue v;
    std::string text;
    if (Member(c, "name", v) && (v.type & 0x3f) == ValueString && v.data) {
        const char* s = (v.type & ValueManaged) ? *reinterpret_cast<const char**>(v.data)
                                                : reinterpret_cast<const char*>(v.data);
        text = s ? s : "";
    }
    ReleaseValue(v);
    return text;
}

/// SetMember (interface +0x30: pdata, name, value, is a display object).
void SetVisible(const Clip& c, bool visible) {
    GfxValue v;
    v.type = ValueBool;
    v.data = visible ? 1 : 0;
    using F = bool(GUEST_ABI*)(u64, u64, const char*, const GfxValue*, bool);
    const u64 vt = *reinterpret_cast<u64*>(c.oi);
    reinterpret_cast<F>(*reinterpret_cast<u64*>(vt + 0x30))(c.oi, c.data, "visible", &v, true);
}

/// The clip PlayerHUD (health, stamina, items, echoes, insight; a parent of the player's health
/// gauge) shown or hidden. False when no bound gauge leads to it.
bool ShowPlayerHud(bool visible) {
    for (const u64 gauge : Gauges()) {
        const Clip fill = FillOf(gauge);
        if (!fill.oi) {
            continue;
        }
        // Up the parents. Each value stays in place until the walk ends: Scaleform links managed
        // values to each other, so they are never copied.
        GfxValue chain[8];
        int levels = 0;
        bool done = false;
        Clip at = fill;
        for (; levels < 8 && !done; ++levels) {
            GfxValue& parent = chain[levels];
            if (!Member(at, "parent", parent) || (parent.type & 0x3f) != ValueDisplayObject || !parent.data ||
                parent.oi != at.oi) {
                ReleaseValue(parent);
                break;
            }
            at = Clip{parent.oi, parent.data};
            if (Name(at) == "PlayerHUD") {
                SetVisible(at, visible);
                done = true;
            }
        }
        for (int i = levels - 1; i >= 0; --i) {
            ReleaseValue(chain[i]);
        }
        if (done) {
            return true;
        }
    }
    return false;
}

std::atomic<bool> hud_hidden{false}, player_hidden{false};
// The game's thread: what was last set, and when.
bool hud_shown = true, player_drawn = true;
std::chrono::steady_clock::time_point hud_time{}, player_time{};

void HudStep(std::chrono::steady_clock::time_point now) {
    const bool hide = hud_hidden.load();
    if (hide == !hud_shown && (!hide || now - hud_time < Refresh)) {
        return;
    }
    hud_time = now;
    if (ShowPlayerHud(!hide) && hud_shown == hide) {
        hud_shown = !hide;
        std::printf("Photo: HUD %s\n", hide ? "hidden" : "shown");
    }
}

void PlayerStep(std::chrono::steady_clock::time_point now) {
    const bool hide = player_hidden.load();
    if (hide == !player_drawn && (!hide || now - player_time < Refresh)) {
        return;
    }
    player_time = now;
    using Find = u64(GUEST_ABI*)(u32, u32, void*);
    if (!Game<Find>(FindCharacter)(PlayerEntity, 0, nullptr)) {
        return;
    }
    using F = void(GUEST_ABI*)(void*, u32, u32);
    Game<F>(SetDrawEnable)(nullptr, PlayerEntity, hide ? 0 : 1);
    if (player_drawn == hide) {
        player_drawn = !hide;
        std::printf("Photo: the player's model %s\n", hide ? "hidden" : "shown");
    }
}

} // namespace

void PatchImage(unsigned char* base, std::uint64_t) {
    image = base;
}

void NoteGauge(void* gauge) {
    std::lock_guard<std::mutex> g(gauge_lock);
    const u64 at = reinterpret_cast<u64>(gauge);
    if (std::find(gauges.begin(), gauges.end(), at) == gauges.end() && gauges.size() < 1024) {
        gauges.push_back(at);
    }
}

std::vector<std::uint64_t> Gauges() {
    std::lock_guard<std::mutex> g(gauge_lock);
    return gauges;
}

void GameStep() {
    if (!image) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    HudStep(now);
    PlayerStep(now);
}

void ToggleHud() {
    hud_hidden.store(!hud_hidden.load());
}

void TogglePlayer() {
    player_hidden.store(!player_hidden.load());
}

} // namespace BbPhoto
