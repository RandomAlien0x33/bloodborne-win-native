// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_cheats.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bbport_game_menu.h"
#include "bbport_settings.h"

namespace BbCheats {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u64 = std::uint64_t;

/// One write of a cheat (eboot 1.09 vaddrs = image offsets). A hook replaces the game's
/// instructions (`off`) with `on`, mostly a jump to the cheat's code; that code goes in the zero
/// bytes after the text segment's end ("cave": no `off`), written once and left there.
struct Write {
    u64 at;
    const char* on;
    const char* off; ///< null: cave code
    /// The cave code's RIP-relative operand to the player slot: its disp32 offset and the
    /// instruction's end (-1: none).
    int slot_disp = -1, slot_end = 0;
};
struct Code {
    std::vector<Write> writes; ///< caves first, then hooks; switched off in reverse order
};

/// The cave: after the text segment (0x50d96dc), in its last page, before the data segment.
constexpr u64 CavePage = 0x50db000, CaveStart = 0x50db800, CaveEnd = 0x50db8a0;
/// The player's pointer (one-hit kills spare the player): past the data segment's end
/// (0x56d30f4), in its last page.
constexpr u64 PlayerSlot = 0x56d3ff8;

// BbSettings::Cheats order. The player slot is reached RIP-relative: `mov [rip+slot], rax`
// (+ 3 nops) and `cmp rbx, [rip+slot]`.
const Code codes[BbSettings::CheatCount] = {
    // Infinite health: the player's HP update stores 0x7fffffff instead of passing it on.
    {{{0x1bfc5f7, "C787F8000000FFFFFF7F90", "8B97F80000004189542404"}}},
    // Infinite stamina
    {{{0x50db836, "8B88380100008988340100008B80340100004189442434E98FC081FC", nullptr},
      {0x18f78db, "E9563F7E0390", "8B8034010000"}}},
    // Infinite Blood Echoes
    {{{0x50db800, "B9FFE0F505898A94000000E9914A82FC", nullptr},
      {0x190029b, "E960B57D0390", "898A94000000"}}},
    // Infinite Insight
    {{{0x50db889, "BEFFE0F50589B3840000008BB384000000E91B6782FC", nullptr},
      {0x1901fb4, "E9D0987D0390", "8BB384000000"}}},
    // Infinite items
    {{{0x50db877, "4189C441BC1400000044896308E9D4DC3FFC", nullptr},
      {0x14d9556, "E91C23C0039090", "4189C444896308"}}},
    // One-hit kills; the first cave code keeps the player's pointer (spared).
    {{{0x50db810, "48890500000000909090" "8B88F80000008988F80000008B88F800000041894C2418E981C081FC", nullptr, 3, 7},
      {0x50db852, "39F00F4EF0534889FB4881EBF0000000483B1D000000007406" "5BE9111C93FC5BE90D1C93FC", nullptr, 19, 23},
      {0x18f78b1, "E95A3F7E0390", "8B88F8000000"},
      {0x1a0d47b, "E9D2E36C03", "39F00F4EF0"}}},
};

u8* image = nullptr;
bool checked = false;
bool available[BbSettings::CheatCount] = {};
bool applied[BbSettings::CheatCount] = {};
std::vector<u8> on_bytes[BbSettings::CheatCount][4], off_bytes[BbSettings::CheatCount][4];

std::vector<u8> Bytes(const char* hex) {
    std::vector<u8> out;
    for (const char* p = hex; p && p[0] && p[1]; p += 2) {
        out.push_back(u8(std::stoi(std::string(p, 2), nullptr, 16)));
    }
    return out;
}

/// Makes [at, at + size) writable, or back as it was (`restore`: what the first call returned).
unsigned long Protect(u64 at, std::size_t size, bool writable, unsigned long restore = 0) {
    const u64 page = 4096;
    const u64 start = at & ~(page - 1), end = (at + size + page - 1) & ~(page - 1);
#ifdef _WIN32
    DWORD old = 0;
    if (!VirtualProtect(image + start, end - start, writable ? PAGE_EXECUTE_READWRITE : DWORD(restore), &old)) {
        std::printf("Cheats: VirtualProtect at %#llx failed (%lu)\n", (unsigned long long)at, GetLastError());
    }
    if (!writable) {
        FlushInstructionCache(GetCurrentProcess(), image + start, end - start);
    }
    return old;
#else
    (void)restore;
    mprotect(image + start, end - start, PROT_READ | PROT_EXEC | (writable ? PROT_WRITE : 0));
    return 0;
#endif
}

/// Writes code another thread may be running: a jump to itself goes in first (a two-byte
/// store), then the rest, then the first two bytes; a thread reaching it meanwhile waits there.
void Patch(u64 at, const std::vector<u8>& bytes) {
    u8* site = image + at;
    if (std::memcmp(site, bytes.data(), bytes.size()) == 0) {
        return;
    }
    const unsigned long old = Protect(at, bytes.size(), true);
    if (bytes.size() > 2) {
        __atomic_store_n(reinterpret_cast<u16*>(site), u16(0xfeeb), __ATOMIC_SEQ_CST); // jmp $
        std::memcpy(site + 2, bytes.data() + 2, bytes.size() - 2);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    u16 head = 0;
    std::memcpy(&head, bytes.data(), bytes.size() >= 2 ? 2 : 1);
    if (bytes.size() >= 2) {
        __atomic_store_n(reinterpret_cast<u16*>(site), head, __ATOMIC_SEQ_CST);
    } else {
        site[0] = bytes[0];
    }
    Protect(at, bytes.size(), false, old);
}

/// The cave's page executable: the loader leaves it inaccessible (past the text segment's end).
void OpenCave() {
#ifdef _WIN32
    DWORD old = 0;
    VirtualProtect(image + CavePage, 4096, PAGE_EXECUTE_READ, &old);
#else
    mprotect(image + CavePage, 4096, PROT_READ | PROT_EXEC);
#endif
}

/// Once, with the game running: each cheat's bytes as the game has them (off, or a cave still
/// zero or already ours).
void Check() {
    checked = true;
    for (int c = 0; c < BbSettings::CheatCount; ++c) {
        const auto& writes = codes[c].writes;
        bool ok = writes.size() <= 4;
        for (std::size_t w = 0; ok && w < writes.size(); ++w) {
            const Write& write = writes[w];
            std::vector<u8> on = Bytes(write.on);
            if (write.slot_disp >= 0) {
                const std::int64_t disp = std::int64_t(PlayerSlot) - std::int64_t(write.at + write.slot_end);
                const std::int32_t disp32 = std::int32_t(disp);
                std::memcpy(on.data() + write.slot_disp, &disp32, 4);
            }
            std::vector<u8> off = write.off ? Bytes(write.off) : std::vector<u8>(on.size(), 0);
            const u8* site = image + write.at;
            ok = on.size() == off.size() && !on.empty() &&
                 (write.off || (write.at >= CaveStart && write.at + on.size() <= CaveEnd)) &&
                 (std::memcmp(site, off.data(), off.size()) == 0 || std::memcmp(site, on.data(), on.size()) == 0);
            on_bytes[c][w] = std::move(on);
            off_bytes[c][w] = std::move(off);
        }
        available[c] = ok;
        if (!ok) {
            std::printf("Cheats: %s: the game's code differs, off\n", BbSettings::Cheats[c].key);
        }
    }
}

} // namespace

void PatchImage(unsigned char* base, std::uint64_t size) {
    if (size >= CaveEnd && size > PlayerSlot + 8) {
        image = base;
    }
}

bool Available(int cheat) {
    return !image || !checked || available[cheat];
}

void Poll() {
    // Not before the game's in-game step has run: the presenter shows frames while the loader
    // still relocates and protects the image (it made the cave inaccessible again after an early
    // check, and the next cheat's write faulted there).
    if (!image || BbGameMenu::StepAgeMs() < 0) {
        return;
    }
    if (!checked) {
        OpenCave();
        Check();
    }
    auto& s = BbSettings::Get();
    for (int c = 0; c < BbSettings::CheatCount; ++c) {
        const bool want = s.cheats[c].load() && available[c];
        if (want == applied[c]) {
            continue;
        }
        OpenCave();
        const auto& writes = codes[c].writes;
        if (want) {
            for (std::size_t w = 0; w < writes.size(); ++w) {
                Patch(writes[w].at, on_bytes[c][w]);
            }
        } else {
            for (std::size_t w = writes.size(); w-- > 0;) {
                if (writes[w].off) {
                    Patch(writes[w].at, off_bytes[c][w]);
                }
            }
        }
        applied[c] = want;
        std::printf("Cheats: %s %s\n", BbSettings::Cheats[c].key, want ? "on" : "off");
    }
}

} // namespace BbCheats
