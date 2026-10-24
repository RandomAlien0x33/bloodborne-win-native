// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the game's frame rate set free by hooks in its code, not by the community's frame rate
// patches: its frame pacer no longer waits for a 30 FPS frame, its step
// gets the frame's real time, and what it ties to a 30 FPS frame (a frame's time, per-frame
// steps, smoothing and damping) reads values recomputed from that time every frame. The limit
// (the menus' "Frame rate") is the pacer's frame time target: it changes while the game runs.
#pragma once

#include <cstdint>

namespace BbFrameRate {
/// Installs the hooks (bbgpu_patch_image, before the game runs). BB_FRAME_HOOKS=0: none (the
/// launcher then applies the frame rate patches).
void PatchImage(unsigned char* image, std::uint64_t size);
/// The hooks run the game's frame rate: video out shows a frame as soon as it comes.
bool Unlocked();
/// F3: 30 FPS for the moment (not saved), on and off; 30 FPS is what the game was made for.
void ToggleLimit30();
} // namespace BbFrameRate
