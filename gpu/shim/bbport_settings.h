// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>
#include <string>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
/// FSR 4, FSR 4.1.1 or DLSS: one frame's inputs to a separate upscaler, which writes the
/// output image (TemporalUpscaler::RecordFsr4); FSR 3.1 and TAA are recorded in place.
inline bool IsFrameUpscaler(int upscaler) {
    return IsFsr4(upscaler) || upscaler == UpscalerDlss;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
/// The port's menu language: English, Russian, or the game's own (Game: as the game's menu texts
/// are, the system's until the game has shown one).
enum class MenuLanguage { English, Russian, Game };
/// The game's language as its menu texts show it (BbGameMenu: Cyrillic or not).
enum class GameLanguage : int { Unknown, English, Russian };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    const char* label_ru;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", "Хроматическая аберрация", true},
    {"effect_dof", "Depth of field (DoF)", "Глубина резкости (DoF)", true},
    {"effect_motion_blur", "Motion blur", "Размытие в движении", true},
    {"effect_ssao", "Ambient occlusion (SSAO)", "Затенение SSAO", true},
    {"effect_game_aa", "Game's own anti-aliasing", "Собственное сглаживание игры", true},
    {"effect_dynamic_shadows", "Shadows from dynamic lights", "Тени от динамических источников", true},
    {"effect_ssr", "Screen-space reflections (not in original game)", "Отражения SSR (не было в игре)", false},
    {"skip_intro", "Skip startup intros", "Пропуск заставок при запуске", false},
    {"debug_camera", "Free camera (Cross + L3)", "Свободная камера (Cross + L3)", false},
    {"debug_menu", "Debug menu (requires font files)", "Debug menu (нужны файлы шрифтов)", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Cheats (bbport_cheats.cpp), switched at once: ini key, menu
/// label (short: the game's name column is narrow), its help line. All off by default.
struct Cheat {
    const char* key;
    const char* label;
    const char* label_ru;
    const char* help;
    const char* help_ru;
};
inline constexpr Cheat Cheats[] = {
    {"cheat_health", "Infinite health", "Бесконечное здоровье", "Health never runs out",
     "Здоровье не кончается"},
    {"cheat_stamina", "Infinite stamina", "Бесконечная выносливость", "Stamina never runs out",
     "Выносливость не кончается"},
    {"cheat_blood_echoes", "Infinite Blood Echoes", "Бесконечные отголоски",
     "Blood Echoes are set to 99,999,999", "Отголосков крови становится 99 999 999"},
    {"cheat_insight", "Infinite Insight", "Бесконечное озарение", "Insight is set to 99,999,999",
     "Озарения становится 99 999 999"},
    {"cheat_items", "Infinite items", "Бесконечные предметы", "A used item's count is set to 20",
     "Предметов после использования остаётся 20"},
    {"cheat_one_hit_kill", "One-hit kills", "Убийство с одного удара", "Enemies die from one hit",
     "Враги умирают с одного удара"},
};
inline constexpr int CheatCount = int(sizeof(Cheats) / sizeof(Cheats[0]));
/// Live output resolutions: the upscaler's output and the UI host targets.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160};
inline constexpr int OutputCount = 4;
/// bbport.ini memory_model, read by the launcher at start: scripts/run_game.py MEMORY_MODELS sets
/// each one's environment under the same key. A new model: its row here (the menus list them in
/// this order) and its entry there. Integrated GPU (direct) stays last: discrete GPUs are not
/// offered it (the menus show the models before it).
enum MemoryModel : int {
    MemoryClassic = 0,
    MemoryHybrid,
    MemoryExperimental1,
    MemoryDirect,
    MemoryModelCount
};
struct MemoryModelInfo {
    const char* key; ///< bbport.ini memory_model
    const char* label;
    const char* label_ru;
};
inline constexpr MemoryModelInfo MemoryModels[MemoryModelCount] = {
    {"classic", "Classic", "Классический"},
    {"hybrid", "Hybrid", "Гибрид"},
    {"experimental1", "Experimental 1", "Экспериментальный 1"},
    {"direct", "Integrated GPU", "Встроенная графика"},
};
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size
/// bbport.ini fps: the frame rate limit (bbport_frame_rate), applied at once.
enum FrameRate : int { Fps30 = 0, Fps60, Fps90, Fps120, Fps144, FpsUnlimited, FrameRateCount };
struct FrameRateInfo {
    const char* key; ///< bbport.ini fps
    int rate;        ///< frames per second; 0: as many as the game allows
    const char* label;
    const char* label_ru;
};
inline constexpr FrameRateInfo FrameRates[FrameRateCount] = {
    {"30", 30, "30 (as on PS4)", "30 (как на PS4)"},
    {"60", 60, "60", "60"},
    {"90", 90, "90", "90"},
    {"120", 120, "120", "120"},
    {"144", 144, "144", "144"},
    {"uncap", 0, "Unlimited", "Без ограничения"},
};

struct Values {
    std::atomic<MenuLanguage> menu_language{MenuLanguage::Game};
    /// The language MenuLanguage::Game uses until the game's is known (the launcher's or system's).
    std::atomic<bool> system_russian{false};
    std::atomic<GameLanguage> game_language{GameLanguage::Unknown};
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    /// The mouse turns the camera while the game window has focus (bbport_mouse_camera.cpp);
    /// degrees per mouse count = 0.022 x sensitivity (the scale of Source games).
    std::atomic<bool> mouse_camera{true};
    std::atomic<float> mouse_sensitivity{1.0f};
    std::atomic<bool> mouse_invert_y{false};
    /// Save copies (bbport_save_menu.cpp): how many the player's copies are kept, and the
    /// keys (SDL key names, modifiers with '+': "F5", "Ctrl+S"; empty: none). Read at start.
    std::atomic<int> save_copies{15};
    std::string quicksave_key{"F5"}, quickload_key{"F8"};
    /// The settings menu's position (fraction of the screen), -1 until it is moved.
    std::atomic<float> menu_x{-1.0f}, menu_y{-1.0f};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    /// Applied at once (BbCheats::Poll).
    std::atomic<bool> cheats[CheatCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> memory_model{MemoryClassic}; ///< MemoryModel, on restart
    std::atomic<int> frame_rate{Fps120};           ///< FrameRate, at once
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Borderless fullscreen window at the desktop size (F11 toggles; BB_FULLSCREEN overrides).
    std::atomic<bool> fullscreen{false};
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// DLSS (upstream's gpu/dlss_bridge, or the driver's NGX on Windows: vk_dlss_ngx) is ready, or
    /// why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    /// The game runs on an integrated GPU (set with the device): the menus offer memory_model
    /// direct ("Integrated GPU") there only, or when bbport.ini has it already.
    std::atomic<bool> integrated_gpu{false};
    std::atomic<const char*> dlss_problem{nullptr};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_memory_model = MemoryClassic;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
};

Values& Get();
/// The port's texts in Russian (menu_language, or the game's language for MenuLanguage::Game).
bool Russian();
/// Localized overlay text; the Language selector itself stays in English.
const char* MenuText(const char* english, const char* russian);

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 or DLSS uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411, bool dlss);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace BbSettings
