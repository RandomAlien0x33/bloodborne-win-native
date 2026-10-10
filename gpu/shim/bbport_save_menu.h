// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: saving and loading copies of the save from the menus and with keys (the files are
// handled in src/runtime_savecopies.c). The game's pause menu (bbport_game_menu.cpp), the port's
// overlay and the keys go through here; a short message on screen tells what happened.
//
// A load does not restart anything: the game goes to the title screen by its own "Exit Game", the
// save it writes on the way is dropped, the copy goes in as the title screen reads the save, and
// "Continue" starts from it.

#pragma once

#include <string>
#include <vector>
#include "bbport_save_copies.h"

union SDL_Event;

namespace BbSaveMenu {

/// A copy of the save now, in the background.
void Save();
/// A copy is being made.
bool Busy();
/// The copies, newest first: the player's (manual) ones only, or all.
std::vector<RuntimeSaveCopy> List(bool manual_only = false);
/// Loads copy `name` (empty: the game's own save as it is on disk) now: the game thread of the
/// pause menu calls it. False when it cannot start (the message says why).
bool Load(const std::string& name);
/// The same from another thread (the overlay, the keys): done on the game's next in-game step with
/// the character in the world; asked for on a loading screen, it waits for the screen to end.
void LoadLater(const std::string& name);
/// A load can be asked for now: none is under way.
bool CanLoad();
/// The game's thread, after each in-game step (bbport_game_menu.cpp): starts a load asked for by
/// LoadLater once the step is `settled` (not leaving the world).
void GameStep(bool settled);

/// Present thread, every frame: drops a load no in-game step took and gives up one the game did
/// not act on.
void Poll();

/// Window thread: the save and load keys (bbport.ini quicksave_key / quickload_key). The load
/// key loads the newest of the player's copies after a second press. True when consumed.
bool HandleKey(const SDL_Event& event);

/// Shows a short message (in the menu language).
void Notice(const char* english, const char* russian);

/// The message to show (empty: none); `alpha` fades it out.
std::string Message(float* alpha);

} // namespace BbSaveMenu
