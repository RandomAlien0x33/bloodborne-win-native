// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: screenshot helpers, anywhere in the game: the player's HUD hidden (H) and the player's
// model hidden (G).

#pragma once

#include <cstdint>
#include <vector>

namespace BbPhoto {

/// The game's image (bbgpu_patch_image).
void PatchImage(unsigned char* image, std::uint64_t size);
/// A HUD gauge bound to its bars (0x1b580e0, the game's thread): the way to the HUD's clips.
void NoteGauge(void* gauge);
/// Every gauge bound since the start (the arena colours the boss bars through them).
std::vector<std::uint64_t> Gauges();
/// The game's thread, after each in-game step.
void GameStep();

/// The player's HUD (health, items, echoes, insight) hidden or shown again.
void ToggleHud();
/// The player's model hidden or drawn again.
void TogglePlayer();

} // namespace BbPhoto
