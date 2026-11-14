// SPDX-License-Identifier: GPL-2.0-or-later
// bbport tour: see bbport_tour.h. The points come from tools/tour/make_points.py (the player's own
// game files): the lamps' arrival points and the enemies (or regions) of every map, with their
// positions, and event flags to set or clear first (the bosses' "defeated" flags are cleared: they
// stand in their arenas again). A new map is reached by the game's map warp (as
// WarpNextStage_Bonfire, the Hunter's Dream headstones); points inside it by putting the character
// there, put back while it falls (the ground streams in around it). At each point the camera is
// levelled, then turns round (right stick, BB_PAD_FILE) until nothing new is compiled. Saves are
// frozen and health is infinite for the whole tour. The point reached is kept in
// BB_TOUR_PROGRESS: a restarted tour goes on from there.

#include "bbport_tour.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <algorithm>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif
#include <SDL3/SDL.h>

#include "bbport_game_menu.h"
#include "bbport_save_menu.h"
#include "bbport_settings.h"
#include "bbport_toggles.h"

extern "C" void runtime_file_saves_freeze(int on);
extern "C" std::uint64_t runtime_pad_stick_idle_ms(int right);
extern "C" void runtime_pad_sticks(std::uint8_t out[4]);
extern "C" void runtime_pad_take_l3(int on);
extern "C" unsigned runtime_pad_l3_presses(void);
extern "C" void runtime_pad_take_touchpad(int on);
extern "C" int runtime_pad_touchpad_down(void);

namespace BbTour {
namespace {

// The game calls and is called with the System V ABI, not the host's Win64 one.
#ifdef _WIN32
#define GUEST_ABI __attribute__((sysv_abi))
#else
#define GUEST_ABI
#endif

using u64 = std::uint64_t;
using u32 = std::uint32_t;

/// The game's warp request ([0x5556678]: +0xc the map, area << 24 | block << 16, +0x10 the
/// entity to arrive at) and its start (0x13cde30), as WarpNextStage_Bonfire (0x132e050) does it.
constexpr u64 WarpRequest = 0x5556678;
constexpr u64 WarpStart = 0x13cde30;
/// The scripts' SetEventFlag (0x132e5a0: this, flag, on).
constexpr u64 SetEventFlag = 0x132e5a0;
/// The scripts' Warp (Lua 0x132df50: this, character, target): 0x13cb3d0(character, target, 0)
/// puts the character at the target of the loaded map, a region (POINT_PARAM: position, angle)
/// or else a part (an enemy, an object) found by its entity id, through the character's own
/// move (vtable +0x5f8): its physics goes along. An unknown target leaves it where it is.
constexpr u64 MoveToEntity = 0x13cb3d0;
/// The character of an entity id (0x13c97a0: entity, unused, optional out), null if none.
constexpr u64 FindCharacter = 0x13c97a0;
/// The loaded map's region of an entity id (0x154c780: unused, entity), null if none.
constexpr u64 FindRegion = 0x154c780;
constexpr u32 PlayerEntity = 10000;
/// The scripts' ResetCamAngle (Lua 0x1337bc0, no arguments): zeroes the player camera's angle
/// ([[[0x553e878]+0x60]+0x58]+0x280 or +0x80, vector at +0x40): behind the character, level.
constexpr u64 ResetCamAngle = 0x1337bc0;

/// Seconds: settled before the first warp, after an arrival before looking round, looking round
/// (a full turn takes the camera ~2.3 s), and how long a warp may go untaken.
constexpr double FirstWait = 5.0, WarpTimeout = 20.0;
/// Looking round at a point: at least min_look (BB_TOUR_MIN_LOOK, about a full turn of the camera
/// at the turn speed), then on as soon as nothing new has been compiled for QuietTime (the GPU has
/// met what this view draws), at most MaxLook.
double min_look = 5.0;
/// The same at a boss (kind "boss", BB_TOUR_BOSS_LOOK): time to look at it.
double boss_look = 20.0;
constexpr double QuietTime = 1.5, MaxLook = 12.0;
/// The right stick while looking round (BB_TOUR_TURN, 129..255: 128 is the centre, 255 the full
/// speed, about 2.3 s a turn).
int turn = 225;
/// Before turning: the right stick held up or down (BB_TOUR_TILT, ry: below 128 up) for
/// BB_TOUR_TILT_TIME seconds, levelling the camera.
int tilt = 60;
double tilt_time = 0.0;
bool turning = false;
/// Puts at the current point (the first one and those after a fall).
int puts = 0;
/// After a map's load, before the first move inside it.
constexpr double ArriveWait = 2.0;
/// After a move: the character must stand this long without falling before the tour looks
/// round. The ground there (collision) streams in around the character: one put down before it
/// has falls through, and is put back.
constexpr double HoldTime = 1.0;
/// Height above the point it is held at (enemies' and regions' positions are at foot level).
constexpr float HoldLift = 0.05f;
/// Falling: this far below the point the character is put back, at most MaxPuts times in all.
constexpr float FallDrop = 2.0f;
constexpr int MaxPuts = 8;


struct Point {
    /// The map mAA_BB_CC_DD (CC, DD: a chalice dungeon's part and layout; 0 elsewhere).
    int area = 0, block = 0, part = 0, layout = 0, entity = 0;
    int Map() const {
        return area << 24 | (block & 0xff) << 16 | (part & 0xff) << 8 | (layout & 0xff);
    }
    std::string MapName() const {
        char name[24];
        std::snprintf(name, sizeof(name), "m%02d_%02d_%02d_%02d", area, block, part, layout);
        return name;
    }
    std::string kind;
    /// Where the entity is in the map's file (the same frame as the game's positions); the
    /// tour puts the character there itself when known (has_at).
    float at[3] = {};
    bool has_at = false;
    std::string title; ///< shown on the screen there (after a tab in the points file)
};

enum class State { Start, Warp, Leaving, Loading, Arrived, Settle, Looking, Done };

u64 image_base = 0;
std::vector<Point> points;
std::vector<std::pair<u32, bool>> flags; ///< set (true) or cleared before the tour
/// Read by the pipeline cache's threads too (OnCacheBlob).
std::atomic<std::size_t> next{0};
State state = State::Start;
/// Between the first warp and the end: the cutscene watcher may press Options.
std::atomic<bool> touring{false};
std::chrono::steady_clock::time_point since;
std::string progress_path, pad_path;
int deaths = 0;
/// The map loaded by the last warp (area << 8 | block), -1 when not known (start, after a death).
int current_map = -1;
/// Looking: compiles when it began and at the last change, and when that was.
u64 point_compiles = 0, last_compiles = 0;
std::chrono::steady_clock::time_point quiet_since;

double Seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

void Enter(State s) {
    state = s;
    since = std::chrono::steady_clock::now();
}

void Pad(const char* text) {
    if (pad_path.empty()) {
        return;
    }
    // The pad file is read again when its time or its size changes: the two texts differ in size.
    if (std::FILE* f = std::fopen(pad_path.c_str(), "w")) {
        std::fputs(text, f);
        std::fclose(f);
    }
}

void SaveProgress() {
    if (progress_path.empty()) {
        return;
    }
    if (std::FILE* f = std::fopen(progress_path.c_str(), "w")) {
        std::fprintf(f, "%zu\n", next.load());
        std::fclose(f);
    }
}

/// A warp to area 29 ignores the rest of the requested map: it loads the current chalice dungeon,
/// kept in event flags. Field 9020 (7 bits) picks the dungeon slot (1 << (6 - slot)); a slot's
/// map is 4 fields of 8 bits from 94000000 + slot * 100000 (area, block, part, layout), as the
/// game writes them when a dungeon is made (0x1ab1f80). Slot 0 is set to the point's layout; the
/// player's own dungeons are only changed in memory (saves are frozen).
constexpr u64 EventFlags = 0x553b100;
constexpr u64 SetEventFlagValue = 0x13d0060; ///< (flags, first flag, bits, value)
constexpr u32 DungeonSlotField = 9020, DungeonSlotMaps = 94000000;

void SetDungeon(const Point& p) {
    const u64 flags_object = *reinterpret_cast<u64*>(image_base + EventFlags);
    if (!flags_object) {
        return;
    }
    using SetValue = void(GUEST_ABI*)(u64, u32, u32, u32);
    const auto set = reinterpret_cast<SetValue>(image_base + SetEventFlagValue);
    set(flags_object, DungeonSlotField, 7, 1u << 6);
    set(flags_object, DungeonSlotMaps, 8, u32(p.area));
    set(flags_object, DungeonSlotMaps + 8, 8, u32(p.block));
    set(flags_object, DungeonSlotMaps + 16, 8, u32(p.part));
    set(flags_object, DungeonSlotMaps + 24, 8, u32(p.layout));
    std::printf("Tour: chalice dungeon slot 0 set to %s\n", p.MapName().c_str());
}

void Warp(const Point& p) {
    if (p.area == 29) {
        SetDungeon(p);
    }
    u32* request = *reinterpret_cast<u32**>(image_base + WarpRequest);
    request[0xc / 4] = u32(p.Map());
    request[0x10 / 4] = u32(p.entity);
    using Start = void(GUEST_ABI*)();
    reinterpret_cast<Start>(image_base + WarpStart)();
}

/// Where the player stands: the position the game's Warp falls back on (0x13cb3d0), the
/// character's [[[[+0x58]+8]+0x3b0]+0x68]+0x1e0. False when a link is not readable.
bool SafeRead(u64 at, void* out, std::size_t size);
bool PlayerPosition(float out[3]) {
    using Find = u64(GUEST_ABI*)(u32, u32, void*);
    u64 at = reinterpret_cast<Find>(image_base + FindCharacter)(PlayerEntity, 0, nullptr);
    for (const u64 offset : {0x58ull, 0x8ull, 0x3b0ull, 0x68ull}) {
        if (at == 0 || !SafeRead(at + offset, &at, sizeof(at))) {
            return false;
        }
    }
    return at != 0 && SafeRead(at + 0x1e0, out, 3 * sizeof(float));
}

/// Points skipped because their character is not in the world.
int skipped = 0;

/// The scripts' IsDisable (Lua 0x1337e70: this, entity): the character is switched off (by the
/// events: another time of the story) or not in its normal state; SetDisable (Lua 0x1332060:
/// this, entity, off) switches it.
constexpr u64 IsCharacterDisabled = 0x1337e70;
constexpr u64 SetCharacterDisabled = 0x1332060;

/// Characters the tour switched on.
int switched_on = 0;

/// A character point whose character the loaded map does not have (0x13c97a0 finds none for its
/// entity id): nothing to look at. One the events switched off (enemies and people of another
/// time of the story) is switched on and looked at. Points without an entity id cannot be told
/// and are kept. Logs where a found character stands, to see how far it is from its placement.
bool Absent(const Point& p) {
    if (p.entity <= 0 || (p.kind != "character" && p.kind != "boss")) {
        return false;
    }
    using Find = u64(GUEST_ABI*)(u32, u32, void*);
    u64 at = reinterpret_cast<Find>(image_base + FindCharacter)(u32(p.entity), 0, nullptr);
    if (at == 0) {
        return true;
    }
    using IsDisabled = bool(GUEST_ABI*)(void*, u32);
    using SetDisabled = bool(GUEST_ABI*)(void*, u32, bool);
    const auto is_disabled = reinterpret_cast<IsDisabled>(image_base + IsCharacterDisabled);
    if (is_disabled(nullptr, u32(p.entity))) {
        reinterpret_cast<SetDisabled>(image_base + SetCharacterDisabled)(nullptr, u32(p.entity),
                                                                          false);
        ++switched_on;
        std::printf("Tour: character %d was switched off: switched on%s\n", p.entity,
                    is_disabled(nullptr, u32(p.entity)) ? " (still off)" : "");
    }
    for (const u64 offset : {0x58ull, 0x8ull, 0x3b0ull, 0x68ull}) {
        if (at == 0 || !SafeRead(at + offset, &at, sizeof(at))) {
            std::printf("Tour: character %d found, its position not readable\n", p.entity);
            return false;
        }
    }
    float live[3]{};
    if (at != 0 && SafeRead(at + 0x1e0, live, sizeof(live))) {
        const float dx = live[0] - p.at[0], dy = live[1] - p.at[1], dz = live[2] - p.at[2];
        std::printf("Tour: character %d at %.1f %.1f %.1f, %.1f m from its placement\n", p.entity,
                    live[0], live[1], live[2], std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return false;
}

/// Puts the player at `at` (keeping its facing) through the character's own move, as the game's
/// Warp does: vtable +0x5f8 (character, position, angle, 1), then flag 2 at +0x1e1.
bool PutPlayer(const float at[3]) {
    using Find = u64(GUEST_ABI*)(u32, u32, void*);
    const u64 chr = reinterpret_cast<Find>(image_base + FindCharacter)(PlayerEntity, 0, nullptr);
    if (chr == 0) {
        return false;
    }
    u64 link = chr;
    for (const u64 offset : {0x58ull, 0x8ull, 0x3b0ull, 0x68ull}) {
        if (link == 0 || !SafeRead(link + offset, &link, sizeof(link))) {
            return false;
        }
    }
    alignas(16) float angle[4]{};
    if (link == 0 || !SafeRead(link + 0x1d0, angle, sizeof(angle))) {
        return false;
    }
    alignas(16) float position[4]{at[0], at[1] + HoldLift, at[2], 1.0f};
    using Move = void(GUEST_ABI*)(u64, float*, float*, u32);
    const u64 vtable = *reinterpret_cast<u64*>(chr);
    reinterpret_cast<Move>(*reinterpret_cast<u64*>(vtable + 0x5f8))(chr, position, angle, 1);
    *reinterpret_cast<unsigned char*>(chr + 0x1e1) |= 2;
    return true;
}

/// The player camera's angle object: [[[0x553e878]+0x60]+0x58]+0x280 (or +0x80). Its +0x50 is
/// the pitch and +0x54 the yaw (radians); +0x40 is what ResetCamAngle zeroes. 0 when not there.
u64 CameraObject() {
    u64 at = 0;
    if (!SafeRead(image_base + 0x553e878, &at, 8) || !at || !SafeRead(at + 0x60, &at, 8) || !at ||
        !SafeRead(at + 0x58, &at, 8) || !at) {
        return 0;
    }
    u64 object = 0;
    if (!SafeRead(at + 0x280, &object, 8) || !object) {
        if (!SafeRead(at + 0x80, &object, 8)) {
            return 0;
        }
    }
    return object;
}

/// BB_TOUR_PITCH_FILE: the camera pitch to hold while looking round (a number; none or no file:
/// left to the game). Read again every half second, so it can be tuned while the tour runs.
std::string pitch_path;
bool HeldPitch(float* out) {
    static auto last = std::chrono::steady_clock::time_point{};
    static bool have = false;
    static float pitch = 0.0f;
    const auto now = std::chrono::steady_clock::now();
    if (!pitch_path.empty() && now - last > std::chrono::milliseconds(500)) {
        last = now;
        const bool had = have;
        const float old = pitch;
        have = false;
        if (std::FILE* f = std::fopen(pitch_path.c_str(), "r")) {
            have = std::fscanf(f, "%f", &pitch) == 1;
            std::fclose(f);
        }
        if (have != had || (have && pitch != old)) {
            if (have) {
                std::printf("Tour: camera pitch held at %.3f\n", pitch);
            } else {
                std::printf("Tour: camera pitch left to the game\n");
            }
            std::fflush(stdout);
        }
    }
    *out = pitch;
    return have;
}

/// The player walks (left stick): the camera does not turn and the tour waits at the point until
/// the stick has been left this long.
constexpr std::uint64_t WalkMs = 2000;

bool WalkingByHand() {
    return runtime_pad_stick_idle_ms(0) < WalkMs;
}

/// The player looks round (right stick, held now): the stick turns the camera instead of the tour
/// (runtime_pad), the tour's timing goes on; the pitch the player chose stays for the rest of
/// the point.
bool pitch_by_hand = false;

void HoldPitch() {
    float pitch = 0.0f;
    if (runtime_pad_stick_idle_ms(1) < 100) {
        pitch_by_hand = true;
    }
    if (pitch_by_hand || !HeldPitch(&pitch)) {
        return;
    }
    if (const u64 object = CameraObject()) {
        *reinterpret_cast<float*>(object + 0x50) = pitch;
    }
}

/// Logs the player camera's angle object (the one ResetCamAngle zeroes at +0x40): 0x30..0x70 as
/// floats, to tell its pitch from its yaw.
void CameraLog(const char* when) {
    const u64 object = CameraObject();
    float f[16]{};
    if (object == 0) {
        return;
    }
    if (!SafeRead(object + 0x30, f, sizeof(f))) {
        return;
    }
    std::printf("Tour: camera %s %#llx:", when, (unsigned long long)object);
    for (const float v : f) {
        std::printf(" %.3f", v);
    }
    std::printf("\n");
    std::fflush(stdout);
}

/// Moves the player to the point of the loaded map: to its position when known, else to its
/// entity (the game's Warp); logs where from.
void MoveTo(const Point& p) {
    puts = 1;
    pitch_by_hand = false;
    if (!p.title.empty()) {
        BbSaveMenu::Notice(p.title.c_str(), p.title.c_str());
    }
    if (p.has_at) {
        float before[3]{};
        const bool known = PlayerPosition(before);
        const bool put = PutPlayer(p.at);
        std::printf("Tour: put at %.1f %.1f %.1f (%s %d)%s from %.1f %.1f %.1f\n", p.at[0],
                    p.at[1], p.at[2], p.kind.c_str(), p.entity, put ? "" : " FAILED",
                    known ? before[0] : 0.0f, known ? before[1] : 0.0f, known ? before[2] : 0.0f);
        std::fflush(stdout);
        return;
    }
    using Region = u64(GUEST_ABI*)(u64, u32);
    const bool region = reinterpret_cast<Region>(image_base + FindRegion)(0, u32(p.entity)) != 0;
    float before[3]{};
    const bool known = PlayerPosition(before);
    using Move = void(GUEST_ABI*)(u32, u32, u32);
    reinterpret_cast<Move>(image_base + MoveToEntity)(PlayerEntity, u32(p.entity), 0);
    std::printf("Tour: move to entity %d (%s, %s) from %.1f %.1f %.1f\n", p.entity, p.kind.c_str(),
                region ? "region" : "part or unknown", known ? before[0] : 0.0f,
                known ? before[1] : 0.0f, known ? before[2] : 0.0f);
    std::fflush(stdout);
}

// ---- Probe: where the character's position lives (one session, to write the tour's moves) ----
// On its own thread (the game goes on), reading through ReadProcessMemory: memory the game frees
// meanwhile fails the read instead of faulting.

/// Reads [at, at + size) of this process; false when any of it is not readable.
bool SafeRead(u64 at, void* out, std::size_t size) {
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, size,
                             &got) && got == size;
#else
    return false;
#endif
}

struct Region {
    u64 begin, end;
};

std::vector<Region> WritableRegions() {
    std::vector<Region> out;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mi{};
    for (u64 a = 0x10000; VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) == sizeof(mi);
         a = u64(mi.BaseAddress) + mi.RegionSize) {
        if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) &&
            !(mi.Protect & PAGE_GUARD)) {
            out.push_back({u64(mi.BaseAddress), u64(mi.BaseAddress) + mi.RegionSize});
        }
    }
#endif
    return out;
}

/// Calls `visit(address, qword)` for every 8-byte aligned qword of `regions` (1 MiB reads).
template <class Visit>
void EachQword(const std::vector<Region>& regions, Visit&& visit) {
    static std::vector<u64> buffer(1 << 17);
    for (const Region& r : regions) {
        for (u64 at = r.begin; at < r.end; at += buffer.size() * 8) {
            const std::size_t n = std::size_t(std::min<u64>(buffer.size(), (r.end - at) / 8));
            if (!SafeRead(at, buffer.data(), n * 8)) continue;
            for (std::size_t i = 0; i < n; ++i) visit(at + i * 8, buffer[i]);
        }
    }
}

/// Float triples within 3 m of where the character stands, then pointer paths to them back to the
/// game's data segment: up to three levels, the last offset below 0x400, the others below 0x1000.
/// Logged; changes nothing.
void ProbeThread(Point p) {
    const auto regions = WritableRegions();
    std::vector<u64> hits;
    {
        static std::vector<float> buffer(1 << 18);
        for (const Region& r : regions) {
            for (u64 at = r.begin; at < r.end && hits.size() < 64; at += (buffer.size() - 2) * 4) {
                const std::size_t n = std::size_t(std::min<u64>(buffer.size(), (r.end - at) / 4));
                if (n < 3 || !SafeRead(at, buffer.data(), n * 4)) continue;
                for (std::size_t i = 0; i + 2 < n; ++i) {
                    const float dx = buffer[i] - p.at[0], dy = buffer[i + 1] - p.at[1],
                                dz = buffer[i + 2] - p.at[2];
                    if (buffer[i] != 0.0f && dx * dx + dy * dy + dz * dz < 9.0f) {
                        hits.push_back(at + i * 4);
                        i += 2;
                    }
                }
            }
        }
    }
    std::printf("Tour probe: %zu positions near %.2f %.2f %.2f\n", hits.size(), p.at[0],
                p.at[1], p.at[2]);
    for (const u64 hit : hits) {
        float f[3]{};
        SafeRead(hit, f, sizeof(f));
        std::printf("Tour probe: %#llx = %.3f %.3f %.3f\n", (unsigned long long)hit, f[0], f[1], f[2]);
    }
    std::fflush(stdout);
    // Level by level: qwords pointing at most `reach` bytes before a target of the level below.
    struct Link {
        u64 at, target;
    };
    const auto pointing = [&](const std::vector<u64>& targets, u64 reach,
                              const std::vector<Region>& where, std::size_t cap) {
        std::vector<u64> sorted = targets;
        std::sort(sorted.begin(), sorted.end());
        std::vector<Link> out;
        EachQword(where, [&](u64 at, u64 v) {
            if (out.size() >= cap) return;
            auto it = std::lower_bound(sorted.begin(), sorted.end(), v);
            if (it != sorted.end() && *it - v < reach) out.push_back({at, *it});
        });
        return out;
    };
    const std::vector<Region> statics{{image_base + 0x5300000, image_base + 0x56d4000}};
    const auto targets = [](const std::vector<Link>& links) {
        std::vector<u64> t;
        for (const Link& l : links) t.push_back(l.at);
        return t;
    };
    const auto level1 = pointing(hits, 0x400, regions, 4000);
    const auto level2 = pointing(targets(level1), 0x1000, regions, 20000);
    std::printf("Tour probe: %zu first-level and %zu second-level pointers\n", level1.size(),
                level2.size());
    const auto chain_from = [&](u64 at, const std::vector<Link>& links) -> const Link* {
        for (const Link& l : links) {
            if (l.at == at) return &l;
        }
        return nullptr;
    };
    const auto read = [](u64 at) {
        u64 v = 0;
        SafeRead(at, &v, sizeof(v));
        return v;
    };
    for (const Link& root : pointing(targets(level1), 0x1000, statics, 100)) {
        const u64 v0 = read(root.at), v1 = read(root.target);
        const Link* l1 = chain_from(root.target, level1);
        if (!l1) continue;
        std::printf("Tour probe: [[image+%#llx]+%#llx]+%#llx\n",
                    (unsigned long long)(root.at - image_base), (unsigned long long)(root.target - v0),
                    (unsigned long long)(l1->target - v1));
    }
    for (const Link& root : pointing(targets(level2), 0x1000, statics, 100)) {
        const Link* l2 = chain_from(root.target, level2);
        const Link* l1 = l2 ? chain_from(l2->target, level1) : nullptr;
        if (!l1) continue;
        const u64 v0 = read(root.at), v2 = read(root.target), v1 = read(l2->target);
        std::printf("Tour probe: [[[image+%#llx]+%#llx]+%#llx]+%#llx\n",
                    (unsigned long long)(root.at - image_base), (unsigned long long)(root.target - v0),
                    (unsigned long long)(l2->target - v2), (unsigned long long)(l1->target - v1));
    }
    std::printf("Tour probe: done\n");
    std::fflush(stdout);
}

void Probe(const Point& p) {
    std::thread(ProbeThread, p).detach();
}

void Log(const char* what, const Point& p) {
    std::printf("Tour: %s point %zu of %zu: %s entity %d (%s)\n", what, next + 1,
                points.size(), p.MapName().c_str(), p.entity, p.kind.c_str());
    std::fflush(stdout);
}

/// Stop and go: L3 (taken from the game during the tour) or F6. While stopped the camera does
/// not turn and the tour stays at the point; going on starts the point's current step again.
std::atomic<bool> stopped{false};
/// Freeze: F7 or the touchpad button (taken from the game during the tour) stops the game itself
/// (its step waits here) until pressed again.
std::atomic<bool> frozen{false};

bool KeyPressed(SDL_Scancode key, bool& was) {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool down = keys && keys[key];
    const bool pressed = down && !was;
    was = down;
    return pressed;
}

bool FreezePressed() {
    static bool f7_was = false, touchpad_was = false;
    const bool f7 = KeyPressed(SDL_SCANCODE_F7, f7_was);
    const bool touchpad_down = runtime_pad_touchpad_down() != 0;
    const bool touchpad = touchpad_down && !touchpad_was;
    touchpad_was = touchpad_down;
    return f7 || touchpad;
}

void Freeze() {
    if (!FreezePressed()) {
        return;
    }
    frozen = true;
    Pad("none");
    BbSaveMenu::Notice("Game frozen: touchpad or F7 goes on", "Game frozen: touchpad or F7 goes on");
    std::printf("Tour: game frozen\n");
    std::fflush(stdout);
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (!FreezePressed());
    frozen = false;
    turning = false;
    since = std::chrono::steady_clock::now();
    quiet_since = since;
    std::printf("Tour: game goes on\n");
    std::fflush(stdout);
}

/// True while the tour is stopped.
bool StopAndGo() {
    static bool f6_was = false;
    static unsigned l3_seen = runtime_pad_l3_presses();
    const unsigned l3 = runtime_pad_l3_presses();
    const bool f6 = KeyPressed(SDL_SCANCODE_F6, f6_was);
    if (l3 != l3_seen || f6) {
        l3_seen = l3;
        stopped = !stopped;
        const char* text = stopped ? "Tour stopped: L3 or F6 goes on" : "Tour goes on";
        BbSaveMenu::Notice(text, text);
        std::printf("Tour: %s\n", stopped ? "stopped" : "goes on");
        std::fflush(stdout);
        if (stopped) {
            Pad("none");
        } else {
            turning = false;
            since = std::chrono::steady_clock::now(); // the current step starts again
            quiet_since = since;
        }
    }
    return stopped;
}

/// True while the player walks (WalkingByHand): the tour waits; the step it was in starts again
/// once the left stick has been left WalkMs.
bool MovingByHand() {
    static bool waiting = false;
    static std::chrono::steady_clock::time_point logged;
    if (WalkingByHand()) {
        const auto now = std::chrono::steady_clock::now();
        if (!waiting || now - logged >= std::chrono::seconds(5)) {
            std::uint8_t sticks[4];
            runtime_pad_sticks(sticks);
            std::printf("Tour: %s (left stick %d %d)\n",
                        waiting ? "still waiting, the player moves" : "waiting while the player moves",
                        sticks[0], sticks[1]);
            std::fflush(stdout);
            logged = now;
        }
        if (!waiting) {
            waiting = true;
            Pad("none");
        }
        return true;
    }
    if (waiting) {
        waiting = false;
        turning = false;
        since = std::chrono::steady_clock::now();
        quiet_since = since;
        std::printf("Tour: goes on after the player's move\n");
        std::fflush(stdout);
    }
    return false;
}

void OnStep(bool settled) {
    if (touring) {
        Freeze();
        if (StopAndGo() || MovingByHand()) {
            return;
        }
    }
    switch (state) {
    case State::Start:
        if (!settled) {
            Enter(State::Start);
        } else if (Seconds() >= FirstWait) {
            runtime_file_saves_freeze(1);
            BbSettings::Get().cheats[0].store(true); // infinite health, for this session only
            using SetFlag = void(GUEST_ABI*)(void*, u32, bool);
            for (const auto& [flag, on] : flags) {
                reinterpret_cast<SetFlag>(image_base + SetEventFlag)(nullptr, flag, on);
            }
            std::printf("Tour: started at point %zu of %zu; saves frozen, infinite health, %zu "
                        "event flags set or cleared\n",
                        next + 1, points.size(), flags.size());
            touring = true;
            runtime_pad_take_l3(1);
            runtime_pad_take_touchpad(1);
            Enter(State::Warp);
        }
        break;
    case State::Warp:
        if (next >= points.size()) {
            Pad("none");
            std::printf("Tour: done, %zu points (%d deaths, %d skipped: nobody there, %d switched on). Saves stay frozen: quit the game.\n",
                        points.size(), deaths, skipped, switched_on);
            std::fflush(stdout);
            BbSaveMenu::Notice("Tour finished: quit the game", "Tour finished: quit the game");
            touring = false;
            runtime_pad_take_l3(0);
            runtime_pad_take_touchpad(0);
            Enter(State::Done);
            break;
        }
        if (settled) {
            const Point& p = points[next];
            if (p.Map() == current_map) {
                Log("move to", p);
                MoveTo(p);
                Enter(State::Settle);
            } else {
                Log("warp to", p);
                Warp(p);
                current_map = p.Map();
                Enter(State::Leaving);
            }
        }
        break;
    case State::Leaving:
        if (!settled) {
            Enter(State::Loading);
        } else if (Seconds() >= WarpTimeout) {
            Log("warp not taken,", points[next]);
            current_map = -1;
            ++next;
            SaveProgress();
            Enter(State::Warp);
        }
        break;
    case State::Loading:
        if (settled) {
            Pad("none");
            Enter(State::Arrived);
        }
        break;
    case State::Arrived:
        // The warp lands at the map's start; the point itself is reached by a move.
        if (!settled) {
            Enter(State::Loading);
        } else if (Seconds() >= ArriveWait) {
            MoveTo(points[next]);
            Enter(State::Settle);
        }
        break;
    case State::Settle: {
        if (!settled) {
            Enter(State::Loading);
            break;
        }
        HoldPitch(); // from the put on: the game turns the camera down meanwhile
        const Point& p = points[next];
        float at[3]{};
        if (p.has_at && PlayerPosition(at) && at[1] < p.at[1] - FallDrop && puts < MaxPuts) {
            PutPlayer(p.at);
            ++puts;
            Enter(State::Settle); // stands from now on, or falls again
            break;
        }
        if (Seconds() >= HoldTime) {
            if (puts > 1) {
                std::printf("Tour: put back %d times\n", puts - 1);
            }
            if (Absent(p)) {
                // A placement the game has not put in the world now (another time of the
                // story, or never): nothing to look at.
                ++skipped;
                Log("nobody there, skipped", p);
                ++next;
                SaveProgress();
                Enter(State::Warp);
                break;
            }
            if (p.kind == "probe") {
                Probe(p);
            }
            // The camera level behind the character first (a put leaves it at any angle).
            using Reset = void(GUEST_ABI*)();
            reinterpret_cast<Reset>(image_base + ResetCamAngle)();
            char stick[16];
            std::snprintf(stick, sizeof(stick), "ry=%d", tilt);
            // The vertical axis held at the centre from now on: a gamepad's own (a resting stick
            // drifts) would tilt the camera.
            Pad(tilt != 128 && tilt_time > 0 ? stick : "ry=128");
            CameraLog("start");
            point_compiles = last_compiles = BbStats::compiles_total.load();
            quiet_since = std::chrono::steady_clock::now();
            turning = false;
            Enter(State::Looking);
        }
        break;
    }
    case State::Looking: {
        if (!settled) {
            // Died (or the game warped on its own): the next point once it has settled again.
            Pad("none");
            ++deaths;
            Log("left early (death?) at", points[next]);
            current_map = -1; // a death goes back to a lamp, maybe of another map
            ++next;
            SaveProgress();
            Enter(State::Loading);
            break;
        }
        HoldPitch();
        if (!turning && Seconds() >= tilt_time) {
            char stick[16];
            std::snprintf(stick, sizeof(stick), "rx=%d ry=128", turn);
            Pad(stick);
            turning = true;
        }
        const u64 compiles = BbStats::compiles_total.load();
        const auto now = std::chrono::steady_clock::now();
        if (compiles != last_compiles) {
            last_compiles = compiles;
            quiet_since = now;
        }
        const double quiet = std::chrono::duration<double>(now - quiet_since).count();
        const bool boss = points[next].kind == "boss";
        const double least = boss ? boss_look : min_look;
        if ((Seconds() >= least && quiet >= QuietTime) || Seconds() >= std::max(MaxLook, least)) {
            CameraLog("end");
            Pad("none");
            float at[3]{};
            const bool known = PlayerPosition(at);
            const Point& p = points[next];
            const bool fell = known && p.has_at && at[1] < p.at[1] - 10.0f;
            std::printf("Tour: at %.1f %.1f %.1f%s, %llu compiles in %.1f s\n", at[0], at[1],
                        at[2], fell ? " (FELL)" : "",
                        (unsigned long long)(compiles - point_compiles), HoldTime + Seconds());
            std::fflush(stdout);
            ++next;
            SaveProgress();
            Enter(State::Warp);
        }
        break;
    }
    case State::Done:
        break;
    }
}

/// Cutscenes (a lamp's flags can start one) and anything else that keeps the game out of the world:
/// past 15 s, Options is pressed every 5 s (it skips a cutscene; on a loading screen it does
/// nothing). Its own thread: the in-game step, and so OnStep, does not run meanwhile.
void CutsceneWatcher() {
    int out_of_world = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!touring || frozen || stopped || BbGameMenu::InPlay()) {
            out_of_world = 0;
            continue;
        }
        if (++out_of_world >= 15 && out_of_world % 5 == 0) {
            std::printf("Tour: out of the world for %d s: Options pressed\n", out_of_world);
            std::fflush(stdout);
            Pad("options");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            Pad("none");
        }
    }
}

bool LoadPoints(const char* path) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        if (line.rfind("flag ", 0) == 0 || line.rfind("unflag ", 0) == 0) {
            std::string word;
            u32 flag = 0;
            if (fields >> word >> flag) {
                flags.emplace_back(flag, word == "flag");
            }
            continue;
        }
        Point p;
        if (const std::size_t tab = line.find('\t'); tab != std::string::npos) {
            p.title = line.substr(tab + 1);
            fields.str(line.substr(0, tab));
        }
        // The map: "AA BB" (mAA_BB_00_00) or "mAA_BB_CC_DD".
        std::string map;
        fields >> map;
        bool have_map = false;
        if (map.size() == 12 && map[0] == 'm') {
            have_map = std::sscanf(map.c_str(), "m%d_%d_%d_%d", &p.area, &p.block, &p.part,
                                   &p.layout) == 4;
        } else if (!map.empty()) {
            p.area = std::atoi(map.c_str());
            have_map = bool(fields >> p.block);
        }
        if (have_map && fields >> p.entity) {
            fields >> p.kind;
            p.has_at = bool(fields >> p.at[0] >> p.at[1] >> p.at[2]);
            points.push_back(p);
        }
    }
    return !points.empty();
}

/// The tour's own copy of the pipeline cache's keys and shader metas (BB_TOUR_PIPELINES) and its
/// list, found.txt: file, point number (0 = before the first point), map, kind, title.
std::filesystem::path pipelines_dir;
std::mutex pipelines_mutex;

} // namespace

void OnCacheBlob(int type, const std::string& name, const std::string& extension,
                 const unsigned char* data, std::size_t size) {
    // 1 = the compiled SPIR-V: made from the game's own code, built again from it at any time.
    if (pipelines_dir.empty() || type == 1) {
        return;
    }
    namespace fs = std::filesystem;
    const fs::path file = pipelines_dir / (name + "." + extension);
    const bool profile = extension == "bin";
    std::scoped_lock lock{pipelines_mutex};
    std::error_code ec;
    // Met in an earlier run: listed once, at the point it was first met. A blob of the same name
    // with other contents (a newer format) replaces the file but is not listed again.
    bool known = false;
    if (!profile && fs::exists(file, ec)) {
        std::ifstream old(file, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(old), {}};
        if (bytes.size() == size && std::memcmp(bytes.data(), data, size) == 0) {
            return;
        }
        known = true;
    }
    const fs::path temp = fs::path{file}.concat(".tmp");
    std::FILE* f = std::fopen(temp.string().c_str(), "wb");
    if (!f) {
        return;
    }
    const bool written = std::fwrite(data, 1, size, f) == size;
    if (std::fclose(f) != 0 || !written) {
        fs::remove(temp, ec);
        return;
    }
    fs::rename(temp, file, ec);
    if (ec || profile || known) {
        return;
    }
    const std::size_t at = next.load();
    const bool on_point = touring && at < points.size();
    std::string where = "start\t\t";
    if (on_point) {
        const Point& p = points[at];
        where = p.MapName() + "\t" + p.kind + "\t" + p.title;
    }
    if (std::FILE* list = std::fopen((pipelines_dir / "found.txt").string().c_str(), "a")) {
        std::fprintf(list, "%s.%s\t%zu\t%s\n", name.c_str(), extension.c_str(),
                     on_point ? at + 1 : 0, where.c_str());
        std::fclose(list);
    }
}

void PatchImage(unsigned char* image, std::uint64_t) {
    image_base = reinterpret_cast<u64>(image);
    const char* points_path = std::getenv("BB_TOUR_POINTS");
    if (!points_path || !LoadPoints(points_path)) {
        std::printf("Tour: no points (BB_TOUR_POINTS=%s): off\n", points_path ? points_path : "");
        return;
    }
    if (const char* p = std::getenv("BB_TOUR_PROGRESS")) {
        progress_path = p;
        std::ifstream in(progress_path);
        std::size_t from = 0;
        if (in >> from) {
            next = from;
        }
    }
    if (const char* t = std::getenv("BB_TOUR_PIPELINES"); t && *t) {
        std::error_code ec;
        std::filesystem::create_directories(t, ec);
        pipelines_dir = t;
    }
    if (const char* t = std::getenv("BB_TOUR_TURN"); t && *t) {
        turn = std::clamp(std::atoi(t), 129, 255);
    }
    if (const char* t = std::getenv("BB_TOUR_MIN_LOOK"); t && *t) {
        min_look = std::clamp(std::atof(t), 1.0, MaxLook);
    }
    if (const char* t = std::getenv("BB_TOUR_TILT"); t && *t) {
        tilt = std::clamp(std::atoi(t), 0, 255);
    }
    if (const char* t = std::getenv("BB_TOUR_TILT_TIME"); t && *t) {
        tilt_time = std::clamp(std::atof(t), 0.0, 5.0);
    }
    if (const char* t = std::getenv("BB_TOUR_PITCH_FILE"); t && *t) {
        pitch_path = t;
    }
    if (const char* t = std::getenv("BB_TOUR_BOSS_LOOK"); t && *t) {
        boss_look = std::clamp(std::atof(t), 1.0, 600.0);
    }
    if (const char* p = std::getenv("BB_PAD_FILE")) {
        pad_path = p;
        Pad("none");
    }
    std::printf("Tour: %zu points from %s, going on from point %zu; camera turn %s (rx=%d, at "
                "least %.1f s a point)\n",
                points.size(), points_path, next + 1, pad_path.empty() ? "off (no BB_PAD_FILE)" : "on",
                turn, min_look);
    BbGameMenu::SetStepListener(&OnStep);
    std::thread(CutsceneWatcher).detach();
}

} // namespace BbTour
