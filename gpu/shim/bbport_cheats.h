// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: cheats (BbSettings::Cheats), switched while the game runs: code written into the
// game's code (Bloodborne 1.09).

#pragma once

#include <cstdint>

namespace BbCheats {

/// The game's image (bbgpu_patch_image, before the loader protects it).
void PatchImage(unsigned char* image, std::uint64_t size);
/// Applies BbSettings' cheat switches that changed (the presenter thread, once a frame).
void Poll();
/// The cheat's code matched the game's (false: off for good, see the log).
bool Available(int cheat);

} // namespace BbCheats
