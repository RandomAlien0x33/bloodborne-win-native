// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_save_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>

#include <SDL3/SDL.h>
#include "bbport_game_menu.h"
#include "bbport_settings.h"

namespace BbSaveMenu {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto MessageTime = std::chrono::seconds(4);
constexpr auto ConfirmTime = std::chrono::seconds(4);
/// A load asked for from the overlay or a key starts on the game's first in-game step with the
/// character settled in the world: at once in play, after the loading screen during one. If none
/// comes by then (the title screen, say), it is dropped.
constexpr auto StepWait = std::chrono::seconds(60);
/// "Exit Game" the in-game step has not taken by then (it stopped: a loading screen) is withdrawn.
constexpr std::int64_t ExitTakeMs = 1000;
/// A load the title screen has not taken by then is given up: saving works again.
constexpr auto TitleWait = std::chrono::seconds(30);

std::atomic<bool> busy{false};
std::mutex mutex; // the message and the requested load
std::string message;
Clock::time_point message_time{};
std::optional<std::string> pending_load; // LoadLater
Clock::time_point pending_since{};       // when it was asked for
Clock::time_point load_started{};        // Load went ahead (mutex)
Clock::time_point load_key_time{}; // the first press of the load key

void Notify(const char* english, const char* russian, const std::string& detail = {}) {
    std::scoped_lock lock{mutex};
    message = BbSettings::MenuText(english, russian);
    if (!detail.empty()) {
        message += " " + detail;
    }
    message_time = Clock::now();
    std::printf("Save copies: %s\n", message.c_str());
}

void NotNow() {
    Notify("A copy is being loaded already", "Копия уже загружается");
}

struct Key {
    SDL_Keycode key = SDLK_UNKNOWN;
    SDL_Keymod mods = SDL_KMOD_NONE;
};
/// "F5", "Ctrl+S", "Shift+Alt+F9": modifiers, then an SDL key name.
Key ParseKey(const std::string& text) {
    Key k;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t plus = text.find('+', start);
        const std::string part = text.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (plus == std::string::npos) {
            k.key = part.empty() ? SDLK_UNKNOWN : SDL_GetKeyFromName(part.c_str());
            break;
        }
        if (SDL_strcasecmp(part.c_str(), "ctrl") == 0) k.mods |= SDL_KMOD_CTRL;
        else if (SDL_strcasecmp(part.c_str(), "shift") == 0) k.mods |= SDL_KMOD_SHIFT;
        else if (SDL_strcasecmp(part.c_str(), "alt") == 0) k.mods |= SDL_KMOD_ALT;
        start = plus + 1;
    }
    if (!text.empty() && k.key == SDLK_UNKNOWN) {
        std::printf("Save copies: unknown key '%s' in bbport.ini\n", text.c_str());
    }
    return k;
}
bool Matches(const Key& k, const SDL_KeyboardEvent& e) {
    if (k.key == SDLK_UNKNOWN || e.key != k.key) {
        return false;
    }
    const auto group = [&](SDL_Keymod m) { return ((k.mods & m) != 0) == ((e.mod & m) != 0); };
    return group(SDL_KMOD_CTRL) && group(SDL_KMOD_SHIFT) && group(SDL_KMOD_ALT);
}

} // namespace

void Save() {
    if (busy.exchange(true)) {
        return;
    }
    Notify("Saving a copy...", "Сохранение копии...");
    std::thread([] {
        char name[64] = {};
        const int keep = BbSettings::Get().save_copies;
        const int made = runtime_saves_copy(1, keep, name, sizeof(name));
        if (made == 0) {
            const auto copies = List(true);
            Notify("Save copy made:", "Копия сохранена:", copies.empty() ? name : copies.front().when);
        } else if (made == 1) {
            Notify("The game has not saved since your last copy: nothing new to copy",
                   "Игра не сохранялась после вашей последней копии: копировать нечего");
        } else {
            Notify("The save could not be copied (see the log)", "Не удалось сохранить копию (см. лог)");
        }
        busy = false;
    }).detach();
}

bool Busy() {
    return busy;
}

std::vector<RuntimeSaveCopy> List(bool manual_only) {
    std::vector<RuntimeSaveCopy> copies(128);
    copies.resize(std::size_t(std::max(0, runtime_saves_list(copies.data(), int(copies.size())))));
    if (manual_only) {
        std::erase_if(copies, [](const RuntimeSaveCopy& c) { return !c.manual; });
    }
    return copies;
}

bool Load(const std::string& name) {
    if (runtime_saves_loading()) {
        return false; // already on the way to the title screen
    }
    if (!BbGameMenu::InPlay()) {
        LoadLater(name); // the world is being left or loaded: after that
        return true;
    }
    while (busy) { // a copy being made finishes first
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (runtime_saves_begin_load(name.c_str()) != 0) {
        Notify("The copy could not be loaded (see the log)", "Не удалось загрузить копию (см. лог)");
        return false;
    }
    Notify("Loading...", "Загрузка...");
    {
        std::scoped_lock lock{mutex};
        load_started = Clock::now();
    }
    BbGameMenu::ExitToTitle();
    return true;
}

bool CanLoad() {
    return !runtime_saves_loading();
}

void LoadLater(const std::string& name) {
    if (!CanLoad()) {
        NotNow();
        return;
    }
    {
        std::scoped_lock lock{mutex};
        pending_load = name;
        pending_since = Clock::now();
    }
    if (!BbGameMenu::InPlay()) {
        Notify("The copy loads as soon as the game has finished loading",
               "Копия загрузится, как только игра закончит загрузку");
    }
}

void GameStep(bool settled) {
    if (!settled) {
        return; // leaving the world (a respawn, a warp): it waits for the step after the loading screen
    }
    std::optional<std::string> name;
    {
        std::scoped_lock lock{mutex};
        name.swap(pending_load);
    }
    if (name) {
        Load(*name);
    }
}

void Poll() {
    const auto now = Clock::now();
    bool dropped = false, stuck = false;
    {
        std::scoped_lock lock{mutex};
        if (pending_load && now - pending_since > StepWait) {
            pending_load.reset(); // the character never came back to the world (the title screen)
            dropped = true;
        }
        if (runtime_saves_loading() && load_started != Clock::time_point{}) {
            // "Exit Game" still waiting while the in-game step no longer runs: the game left the
            // world another way and would take it elsewhere. Or no title screen in time.
            stuck = (BbGameMenu::ExitPending() && BbGameMenu::StepAgeMs() > ExitTakeMs) ||
                    now - load_started > TitleWait;
            if (stuck) {
                load_started = {};
            }
        }
    }
    if (dropped) {
        Notify("The copy was not loaded: the game did not come back to play",
               "Копия не загружена: игра так и не вернулась в игровой процесс");
    }
    if (stuck && runtime_saves_cancel_load()) {
        BbGameMenu::CancelExitToTitle();
        Notify("The game did not go to the title screen: the copy was not loaded, saving works as before",
               "Игра не вышла в главное меню: копия не загружена, сохранение работает как обычно");
    }
}

bool HandleKey(const SDL_Event& event) {
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
        return false;
    }
    static const Key save_key = ParseKey(BbSettings::Get().quicksave_key);
    static const Key load_key = ParseKey(BbSettings::Get().quickload_key);
    if (Matches(save_key, event.key)) {
        Save();
        return true;
    }
    if (!Matches(load_key, event.key)) {
        return false;
    }
    if (!CanLoad()) {
        load_key_time = {};
        NotNow();
        return true;
    }
    const auto copies = List(true);
    const RuntimeSaveCopy* newest = copies.empty() ? nullptr : &copies.front();
    if (!newest) {
        Notify("No save copies yet", "Копий сохранения пока нет");
        return true;
    }
    const auto now = Clock::now();
    if (load_key_time != Clock::time_point{} && now - load_key_time < ConfirmTime) {
        load_key_time = {};
        LoadLater(newest->name);
    } else {
        load_key_time = now;
        Notify("Press again to load the copy", "Нажмите ещё раз, чтобы загрузить копию", newest->when);
    }
    return true;
}

void Notice(const char* english, const char* russian) {
    Notify(english, russian);
}

std::string Message(float* alpha) {
    std::scoped_lock lock{mutex};
    const auto age = Clock::now() - message_time;
    if (message.empty() || age > MessageTime) {
        return {};
    }
    const float left = std::chrono::duration<float>(MessageTime - age).count();
    *alpha = std::min(1.0f, left / 0.5f);
    return message;
}

} // namespace BbSaveMenu
