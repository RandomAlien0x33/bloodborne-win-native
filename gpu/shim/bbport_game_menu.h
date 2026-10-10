// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the port's settings as pages of the game's own System menu ("Display", "Game effects",
// "Game patches" after "Screen/Sound"), built and drawn by the game's menu code: rows are the
// game's list, toggle and slider rows, texts come from a hook on its message lookup, the values live
// in the port and are applied as the player changes them. BB_GAME_MENU=0 leaves the menu as it is.
//
// The pause menu (Options) gets "Saves" after "System": a list like System's with "Save" and
// "Load", Load a list of the game's own save and the player's copies; a load row loads on a
// second press (bbport_save_menu.cpp does the work).
#pragma once

#include <cstdint>
#include <string>

namespace BbGameMenu {
/// Installs the hooks (the loader calls it once, before any game code runs, while the image is
/// still writable). Each hooked function is checked byte for byte against the 1.09 eboot.
void PatchImage(unsigned char* image, std::uint64_t size);

/// Applies what the player changed on the port's pages (each presented frame).
void Poll();

/// A place's name (PlaceName text id) in the game's language, UTF-8; any thread. Empty at first:
/// the game looks it up as it next looks up a text, and later calls have it.
std::string PlaceName(std::uint32_t place);

/// The game's own "Exit Game" (System menu): back to the title screen. Any thread; the game
/// acts on it in its next frame.
void ExitToTitle();
/// The character is in the world (dying too) and the game would take "Exit Game" as from its menu:
/// not on a loading screen, the title screen, or on the way to one. Any thread.
bool InPlay();
/// "Exit Game" was asked for and the game has not taken it yet.
bool ExitPending();
/// Milliseconds since the in-game step last ran (-1: never).
std::int64_t StepAgeMs();
/// Withdraws an "Exit Game" the game has not acted on yet (a load that did not start).
void CancelExitToTitle();
} // namespace BbGameMenu
