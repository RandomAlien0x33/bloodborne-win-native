// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the mouse turns the game's camera directly (bbport_mouse_camera.cpp).
#pragma once
#include <cstdint>

namespace BbMouseCamera {
/// Hooks the game's camera update (game 1.09); before the image is protected.
void PatchImage(unsigned char* image, std::uint64_t size);
/// The hook is in: mouse motion goes to Turn, else the window hands it to the pad as a stick.
bool Active();
/// Adds a turn (radians, the game's angles: pitch grows looking down) for the next camera update.
void Turn(float pitch, float yaw);
} // namespace BbMouseCamera
