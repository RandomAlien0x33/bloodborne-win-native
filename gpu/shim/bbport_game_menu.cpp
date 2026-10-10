// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_game_menu.h"

#ifdef _WIN32
#include <windows.h>
// bbport (Windows): the guest range around the image is the port's own reservation; the runtime
// hands out host memory inside it (runtime_low_map), within rel32 reach of the image.
extern "C" void* runtime_low_map(size_t size, int prot);
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bbport_save_menu.h"
#include "bbport_settings.h"

// The runtime serves its copy of menu/optionsetting.gfx (runtime_file.c).
extern "C" int runtime_file_menu_layout_fixed(void);
// The runtime serves its copy of menu/ingametop.gfx with a fourth cell and icon for "Saves".
extern "C" int runtime_file_top_menu_fixed(void);

// bbport (Windows): the game calls and is called with the System V ABI, not the host's Win64 one.
#ifdef _WIN32
#define GUEST_ABI __attribute__((sysv_abi))
#else
#define GUEST_ABI
#endif

// bbport: no aligned_alloc in the Windows CRT.
static void* AlignedAlloc(std::size_t alignment, std::size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    return std::aligned_alloc(alignment, size);
#endif
}
static void AlignedFree(void* p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}


namespace BbGameMenu {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The game's menu code (eboot 1.09 vaddrs; found from the System menu builder 0x1bb3ad0).
constexpr u64 MsgLookup = 0xf6f340;      ///< (repository, table, category, id) -> UTF-16 text
constexpr u64 AddPageItem = 0x1b4e2a0;   ///< (menu, name+help texts, page factory, out)
constexpr u64 AddActionItem = 0x1b4c9a0; ///< (menu, name+help texts, std::function action, 0)
/// (menu): an empty cell. The pause menu is a grid six cells wide filled in order; its first row
/// is Inventory, Status, System and empty cells up to six.
constexpr u64 AddBlankItem = 0x1be29e0;
/// The in-game step (step, frame time): runs each frame while the character is in the world (also
/// with the pause menu open), not on loading screens or the title screen. It acts on "Exit Game".
constexpr u64 InGameStep = 0x193ac10;
constexpr u64 MakeText = 0x1ae8cc0;      ///< (out 0x40 bytes, category, id): a text entry
constexpr u64 FactoryToFunction = 0x1b6ed90; ///< ({vtable, fn}, out std::function) -> its __f_
constexpr u64 CreatePage = 0x1bb4c70;    ///< (out, arg, content std::function): an options page
constexpr u64 OpenPage = 0x1b20900;      ///< (a, b, layout name, rows fn, 0, 0): the page's content
constexpr u64 ListPush = 0x1b1bbd0;      ///< (value list, {u32 value, text}): up to 32 values
constexpr u64 ListRow = 0x1b29370;   ///< (page, texts, int* value, value list, int* default): pop-up list (the Language page's; 0x1b78ab0, the Chalice search page's, drew under the rows below it here)
constexpr u64 PairPush = 0x1b2c200;  ///< (two-value list, {u8 value, text})
constexpr u64 PairRow = 0x1b2a100;   ///< (page, texts, u8* value, two-value list, u8* default): left/right
constexpr u64 SliderRow = 0x1b2ac00; ///< (page, texts, u8* value 0..10, u8* default)
constexpr u64 FactoryVtable = 0x533d120; ///< a System item's {vtable, page factory fn}
constexpr u64 ContentVtable = 0x5343880; ///< an options page's {vtable, content fn}
constexpr u64 ControlsLayout = 0x4934065; ///< "ControllSetting": six plain rows
/// "LanguageSetting": the layout with pop-up lists (OpenPage builds every System page; on
/// ControllSetting, whose own page has none, an open list was drawn under the rows below it).
constexpr u64 LanguageLayout = 0x4934055;
constexpr u32 TextName = 0xc8, TextHelp = 0xc9; // SP_menu text / SP_one-line help
constexpr u32 ScreenSoundItem = 110001;         // "Screen/Sound" in the System menu: ours follow it
constexpr u32 ControlsTitle = 113020;           // the ControllSetting layout's title ("Controls")
constexpr u32 LanguageTitle = 115010;           // the LanguageSetting layout's title ("Language")
/// ControllSetting (six rows) when the runtime serves its copy of menu/optionsetting.gfx with the
/// rows in the order LanguageSetting has them (runtime_file.c), else LanguageSetting (two rows at a
/// time). BB_GAME_MENU_LAYOUT=ControllSetting/LanguageSetting chooses.
bool ControlsLayoutUsed() {
    static const bool on = [] {
        const char* env = std::getenv("BB_GAME_MENU_LAYOUT");
        if (env && env[0]) {
            return std::strcmp(env, "LanguageSetting") != 0;
        }
        return runtime_file_menu_layout_fixed() != 0;
    }();
    return on;
}
constexpr std::size_t RowsPerPage = 6;          // ControllSetting's rows

const u8 MsgLookupPrologue[] = {0x89, 0xf0, 0x48, 0x8b, 0x77, 0x08, 0x48, 0x8b, 0x34, 0xc6};
const u8 AddPageItemPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
const u8 AddActionItemPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
const u8 AddBlankItemPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
const u8 InGameStepPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};

u64 image_base = 0;
template <typename F>
F Game(u64 va) {
    return reinterpret_cast<F>(image_base + va);
}

// ---- Texts: ids from IdBase up, looked up by the game like its own messages. ----
constexpr u32 IdBase = 0x7f0000;
struct Text {
    std::u16string english, russian;
};
std::vector<Text> texts;

std::u16string Utf16(const char* utf8) {
    std::u16string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8); *p;) {
        u32 c = *p++;
        if (c >= 0xf0) {
            c = (c & 7) << 18 | (p[0] & 0x3f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f);
            p += 3;
        } else if (c >= 0xe0) {
            c = (c & 15) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f);
            p += 2;
        } else if (c >= 0xc0) {
            c = (c & 31) << 6 | (p[0] & 0x3f);
            p += 1;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(char16_t(0xd800 + (c >> 10)));
            out.push_back(char16_t(0xdc00 + (c & 0x3ff)));
        } else {
            out.push_back(char16_t(c));
        }
    }
    return out;
}
u32 AddText(const char* english, const char* russian) {
    texts.push_back({Utf16(english), Utf16(russian)});
    return IdBase + u32(texts.size() - 1);
}
/// The game's own language decides (its "Screen/Sound" text in Cyrillic: Russian, else English);
/// the port's menu language until the game has looked that text up.
enum class GameLanguage { Unknown, English, Russian };
std::atomic<GameLanguage> game_language{GameLanguage::Unknown};

// The memory mode row's help line, and its variant for integrated GPUs, where the list also
// offers "Integrated GPU" (MemoryModelAvailable). 0 until defined.
u32 memory_help = 0, memory_help_integrated = 0;

const char16_t* TextOf(u32 id) {
    if (id == memory_help && memory_help_integrated && BbSettings::Get().integrated_gpu) {
        id = memory_help_integrated;
    }
    const Text& t = texts[id - IdBase];
    const GameLanguage game = game_language.load();
    const bool russian = game == GameLanguage::Unknown
                             ? BbSettings::Get().menu_language == BbSettings::MenuLanguage::Russian
                             : game == GameLanguage::Russian;
    return russian ? t.russian.c_str() : t.english.c_str();
}

// ---- The port's values the game's rows edit (ints: the list rows bind ints). ----
enum Field : int {
    OutputRes, Upscaler, Preset, Sharpness, ShowFps,
    ModelLod, Memory, MouseCamera, MouseSensitivity, MouseInvertY, FirstEffect, FieldCount = FirstEffect + BbSettings::EffectCount
};
int values[FieldCount];  // what the game's rows show and change (list rows bind ints)
u8 bytes[FieldCount];    // the same for toggle and slider rows (they bind bytes)
bool byte_field[FieldCount]; // fields shown by toggle or slider rows
int default_values[FieldCount]; // the port's defaults: what the page's "Default" sets
u8 default_bytes[FieldCount];
int applied[FieldCount]; // what Poll applied last
std::atomic<bool> values_valid{false};
constexpr float SharpnessStep = 0.2f; // the game's slider: 0..10 -> 0.0 .. 2.0
constexpr int SharpnessSteps = 11;
constexpr int LodValues[] = {-2, 0, 1, 2};
// The mouse sensitivity slider (0..10): x 0.022 degrees per count, as in Source games.
constexpr float MouseSensitivities[11] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f};

/// The rows' values of settings `s`.
void Fill(const BbSettings::Values& s, int* values) {
    values[OutputRes] = s.output_res;
    values[Upscaler] = s.upscaler;
    values[Preset] = s.preset;
    // One slider for both: 0 is sharpening off.
    values[Sharpness] =
        s.sharpen ? std::clamp(int(std::lround(s.sharpness / SharpnessStep)), 1, SharpnessSteps - 1) : 0;
    values[ShowFps] = s.show_fps;
    values[ModelLod] = 1;
    for (int i = 0; i < 4; ++i) {
        if (LodValues[i] == s.model_lod) values[ModelLod] = i;
    }
    values[Memory] = s.memory_model;
    values[MouseCamera] = s.mouse_camera;
    values[MouseInvertY] = s.mouse_invert_y;
    values[MouseSensitivity] = 0;
    for (int i = 0; i < 11; ++i) {
        if (std::fabs(MouseSensitivities[i] - s.mouse_sensitivity) <
            std::fabs(MouseSensitivities[values[MouseSensitivity]] - s.mouse_sensitivity)) {
            values[MouseSensitivity] = i;
        }
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        values[FirstEffect + e] = s.effects[e];
    }
}

void LoadValues() {
    Fill(BbSettings::Get(), values);
    for (int f = 0; f < FieldCount; ++f) {
        bytes[f] = u8(values[f]);
    }
    std::memcpy(applied, values, sizeof(values));
    values_valid = true;
}

// ---- Pages and rows ----
enum class Kind { List, Toggle, Slider };
struct Row {
    Field field;
    u32 name, help;
    std::vector<u32> choices; ///< value texts, value = index (a toggle: off, on)
    Kind kind = Kind::List;
};
struct Page {
    u32 name, help;
    std::vector<Row> rows; ///< at most RowsPerPage (the layout's)
};
std::vector<Page> pages;

void DefineTexts() {
    const u32 off = AddText("Off", "Выкл"), on = AddText("On", "Вкл");
    const std::vector<u32> toggle = {off, on};
    const auto page = [](const char* en, const char* ru, const char* help_en, const char* help_ru) {
        const u32 name = AddText(en, ru);
        pages.push_back({name, AddText(help_en, help_ru), {}});
    };
    const auto row = [](Field f, const char* en, const char* ru, const char* help_en,
                        const char* help_ru, std::vector<u32> choices, Kind kind = Kind::List) {
        if (kind == Kind::List && choices.size() == 2) {
            kind = Kind::Toggle; // left/right in place, as the game's own on/off rows
        }
        pages.back().rows.push_back(
            {f, AddText(en, ru), AddText(help_en, help_ru), std::move(choices), kind});
    };

    page("Display", "Изображение", "Resolution, upscaler and sharpness (bbport)",
         "Разрешение, апскейлер и резкость (bbport)");
    // Output and preset are a startup patch unless live resolution changes are on (run.sh /
    // run_game.py set BB_RENDER_RES then): their help says so.
    const bool fixed = BbSettings::FixedRenderSession();
    row(OutputRes, "Output resolution", "Разрешение вывода",
        fixed ? "Size of the final frame and the interface (after a restart)" : "Size of the final frame and the interface",
        fixed ? "Размер готового кадра и интерфейса (после перезапуска)" : "Размер готового кадра и интерфейса",
        {AddText("1280 x 720", "1280 x 720"), AddText("1920 x 1080", "1920 x 1080"),
         AddText("2560 x 1440", "2560 x 1440"), AddText("3840 x 2160", "3840 x 2160")});
    row(Upscaler, "Upscaler", "Апскейлер",
        fixed ? "Applies at once; to or from TAA and Off after a restart" : "Temporal upscaler and anti-aliasing",
        fixed ? "Сразу; переход на TAA и Выкл и обратно после перезапуска" : "Временной апскейлер и сглаживание",
        {off, AddText("FSR 3.1", "FSR 3.1"), AddText("FSR 4", "FSR 4"), AddText("FSR 4.1.1", "FSR 4.1.1"),
         AddText("TAA", "TAA"), AddText("DLSS", "DLSS")});
    row(Preset, "Quality preset", "Пресет",
        fixed ? "Scene resolution relative to the output (after a restart)" : "Scene resolution relative to the output",
        fixed ? "Разрешение сцены относительно вывода (после перезапуска)" : "Разрешение сцены относительно вывода",
        {AddText("Native AA", "Native AA"), AddText("Quality", "Quality"), AddText("Balanced", "Balanced"),
         AddText("Performance", "Performance"), AddText("Ultra Performance", "Ultra Performance")});
    row(Sharpness, "Sharpness", "Резкость", "RCAS after the upscaler: 0 off, 10 strongest",
        "RCAS после апскейлера: 0 выкл, 10 сильнее всего", {}, Kind::Slider);
    row(ShowFps, "FPS counter", "Счётчик FPS", "Frame rate in the top right corner",
        "Частота кадров в правом верхнем углу", toggle);

    page("Additional", "Дополнительно", "More port settings (bbport)", "Другие настройки порта (bbport)");
    row(Memory, "Memory mode", "Режим памяти",
        "Hybrid is faster; if the game crashes, choose Classic. After a restart",
        "Гибрид быстрее; если игра вылетает, выберите Классический. После перезапуска",
        {AddText("Classic", "Классический"), AddText("Hybrid", "Гибрид"),
         AddText("Integrated GPU", "Встроенная графика")});
    memory_help = pages.back().rows.back().help;
    memory_help_integrated =
        AddText("Hybrid is faster, Integrated GPU has no copies; on crashes choose Classic. After a restart",
                "Гибрид быстрее, Встроенная графика без копий; при вылетах выберите Классический. После перезапуска");
    row(MouseCamera, "Mouse camera", "Камера мышью",
        "The mouse turns the camera while the game window has focus",
        "Мышь поворачивает камеру, пока окно игры в фокусе", toggle);
    row(MouseSensitivity, "Mouse sensitivity", "Чувствительность мыши",
        "How far the camera turns per mouse movement", "Насколько камера поворачивается от движения мыши",
        {}, Kind::Slider);
    row(MouseInvertY, "Invert mouse Y", "Инверсия мыши по Y", "Mouse up looks down",
        "Мышь вверх: взгляд вниз", toggle);

    page("Game effects", "Эффекты игры", "Model detail and effects (bbport; after a restart)",
         "Детализация и эффекты (bbport; после перезапуска)");
    row(ModelLod, "Model detail", "Детализация", "Level of detail of models (after a restart)",
        "Детализация моделей (после перезапуска)",
        {AddText("Highest", "Максимальная"), AddText("Game default", "Как в игре"),
         AddText("Lower", "Ниже"), AddText("Lowest", "Минимальная")});
    // Short names for the game's narrow name column (BbSettings::Effects order); the help line
    // has the details.
    struct Short {
        const char *key, *en, *ru, *help_en, *help_ru;
    };
    static const Short shorts[] = {
        {"effect_chromatic_aberration", "Chromatic aberration", "Хром. аберрация",
         "Colour fringes at the frame edges", "Цветные каймы по краям кадра"},
        {"effect_dof", "Depth of field", "Глубина резкости", "Blur of distant and near objects (DoF)",
         "Размытие дальних и близких объектов (DoF)"},
        {"effect_motion_blur", "Motion blur", "Размытие в движении", "Blur when the camera turns",
         "Размытие при повороте камеры"},
        {"effect_ssao", "Ambient occlusion", "Затенение SSAO", "Shading in corners and contacts (SSAO)",
         "Затенение в углах и местах касания (SSAO)"},
        {"effect_game_aa", "Game's own AA", "Сглаживание игры", "The game's own anti-aliasing",
         "Собственное сглаживание игры"},
        {"effect_dynamic_shadows", "Dynamic shadows", "Динамические тени", "Shadows from dynamic lights",
         "Тени от динамических источников света"},
        {"effect_ssr", "SSR reflections", "Отражения SSR", "Screen-space reflections (not in the original)",
         "Экранные отражения (не было в оригинале)"},
        {"skip_intro", "Skip intros", "Пропуск заставок", "Skip the startup logos and intro",
         "Пропуск логотипов и заставки при запуске"},
        {"debug_camera", "Free camera", "Свободная камера", "Toggled with Cross + L3",
         "Включается Cross + L3"},
        {"debug_menu", "Debug menu", "Debug menu", "The game's debug menu (requires font files)",
         "Отладочное меню игры (нужны файлы шрифтов)"},
    };
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        if (pages.back().rows.size() == RowsPerPage) {
            page("Game patches", "Патчи игры", "More game patches (bbport; after a restart)",
                 "Другие патчи игры (bbport; после перезапуска)");
        }
        const auto& effect = BbSettings::Effects[e];
        const Short* name = nullptr;
        for (const Short& s : shorts) {
            if (std::strcmp(s.key, effect.key) == 0) name = &s;
        }
        // A new effect without a short name: its full label.
        const std::string help_en = std::string(name ? name->help_en : effect.label) + " (after a restart)";
        const std::string help_ru =
            std::string(name ? name->help_ru : effect.label_ru) + " (после перезапуска)";
        row(Field(FirstEffect + e), name ? name->en : effect.label, name ? name->ru : effect.label_ru,
            help_en.c_str(), help_ru.c_str(), toggle);
    }
}

// ---- The game's structures, as its own code builds them. ----
constexpr std::size_t TextSize = 0x40;     // text entry (MakeText): text pointer, string, flag
constexpr std::size_t ValueSize = 0x48;    // {u32 value; text}
constexpr std::size_t ListSize = 0x920;    // 32 values + count at +0x908 (+ alignment slack)
constexpr std::size_t PairListSize = 0xb0; // 2 values + count at +0x98 (+ alignment slack)
constexpr std::size_t ListCountOffset = 0x908, PairCountOffset = 0x98;
constexpr std::size_t FunctionSize = 0x28; // std::function: 32-byte buffer, __f_ at +0x20

/// Destroys a std::function the way the game's code does (its __f_ at +0x20).
void DestroyFunction(u8* f) {
    void* impl = *reinterpret_cast<void**>(f + 0x20);
    if (!impl) {
        return;
    }
    using Destroy = void (GUEST_ABI *)(void*, int);
    Destroy destroy = (*reinterpret_cast<Destroy**>(impl))[4];
    destroy(impl, impl != f);
    *reinterpret_cast<void**>(f + 0x20) = nullptr;
}

/// Frees a text entry's string the way the game's code does (heap buffer once capacity >= 8).
void DestroyText(u8* text) {
    if (*reinterpret_cast<u64*>(text + 0x28) >= 8) {
        void* allocator = *reinterpret_cast<void**>(text + 0x30);
        using Free = void (GUEST_ABI *)(void*, void*);
        (*reinterpret_cast<Free**>(allocator))[0x70 / 8](allocator, *reinterpret_cast<void**>(text + 0x10));
    }
    *reinterpret_cast<u64*>(text + 0x28) = 7;
    *reinterpret_cast<u64*>(text + 0x20) = 0;
    *reinterpret_cast<char16_t*>(text + 0x10) = 0;
}
void DestroyTexts(u8* pair) {
    DestroyText(pair);
    DestroyText(pair + TextSize);
}
/// A value list's entries (count at `count_offset`), after the row has copied them.
void DestroyValues(u8* list, std::size_t count_offset) {
    const u64 count = *reinterpret_cast<u64*>(list + count_offset);
    for (u64 i = 0; i < count; ++i) {
        DestroyText(list + i * ValueSize + 8);
    }
}

void MakeTexts(u8* pair, u32 name, u32 help) {
    using Make = void* (GUEST_ABI *)(void*, u32, u32);
    Game<Make>(MakeText)(pair, TextName, name);
    Game<Make>(MakeText)(pair + TextSize, TextHelp, help);
}

/// Memory modes offered: Integrated GPU (direct) on an integrated GPU only, or when it is set.
bool MemoryModelAvailable(int i) {
    const auto& s = BbSettings::Get();
    return i != BbSettings::MemoryDirect || s.integrated_gpu || s.memory_model == BbSettings::MemoryDirect;
}

/// Upscalers this GPU and build can run (known once the device exists, before any menu).
bool UpscalerAvailable(int i) {
    const auto& s = BbSettings::Get();
    return i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
           : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
           : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                                             : true;
}

void AddRows(void* page, const std::vector<Row>& rows) {
    using Push = void (GUEST_ABI *)(void*, void*);
    using AddRow = void (GUEST_ABI *)(void*, void*, int*, void*, const int*);
    using Make = void* (GUEST_ABI *)(void*, u32, u32);
    for (const Row& row : rows) {
        alignas(16) u8 pair[2 * TextSize] = {};
        MakeTexts(pair, row.name, row.help);
        if (row.kind == Kind::Slider) {
            using Slider = void (GUEST_ABI *)(void*, void*, u8*, const u8*);
            Game<Slider>(SliderRow)(page, pair, &bytes[row.field], &default_bytes[row.field]);
            DestroyTexts(pair);
            continue;
        }
        if (row.kind == Kind::Toggle) {
            using PairRowFn = void (GUEST_ABI *)(void*, void*, u8*, void*, const u8*);
            u8* two = static_cast<u8*>(AlignedAlloc(16, PairListSize));
            std::memset(two, 0, PairListSize);
            for (std::size_t i = 0; i < 2; ++i) {
                alignas(16) u8 value[ValueSize] = {};
                value[0] = u8(i);
                Game<Make>(MakeText)(value + 8, TextName, row.choices[i]);
                Game<Push>(PairPush)(two, value);
                DestroyText(value + 8);
            }
            Game<PairRowFn>(PairRow)(page, pair, &bytes[row.field], two, &default_bytes[row.field]);
            DestroyValues(two, PairCountOffset);
            AlignedFree(two);
            DestroyTexts(pair);
            continue;
        }
        u8* list = static_cast<u8*>(AlignedAlloc(16, ListSize));
        std::memset(list, 0, ListSize);
        for (std::size_t i = 0; i < row.choices.size(); ++i) {
            if ((row.field == Upscaler && !UpscalerAvailable(int(i))) ||
                (row.field == Memory && !MemoryModelAvailable(int(i)))) {
                continue; // the list's values are the upscaler numbers, not positions
            }
            alignas(16) u8 value[ValueSize] = {};
            *reinterpret_cast<u32*>(value) = u32(i);
            Game<Make>(MakeText)(value + 8, TextName, row.choices[i]);
            Game<Push>(ListPush)(list, value);
            DestroyText(value + 8);
        }
        Game<AddRow>(ListRow)(page, pair, &values[row.field], list, &default_values[row.field]);
        DestroyValues(list, ListCountOffset);
        AlignedFree(list);
        DestroyTexts(pair);
    }
}

std::atomic<const char16_t*> title_override{nullptr}; // while the game builds one of our pages (its menu thread)
// Called by the game's menu code (guest threads, System V ABI like the game), one set per page.
template <int P>
GUEST_ABI void Rows(void* page, void*) {
    LoadValues();
    AddRows(page, pages[P].rows);
}
template <int P>
GUEST_ABI void* Content(void* a, void* b) {
    using Open = void* (GUEST_ABI *)(void*, void*, const char*, void*, u64, u64);
    title_override = nullptr; // the title was looked up just before
    return Game<Open>(OpenPage)(a, b, Game<const char*>(ControlsLayoutUsed() ? ControlsLayout : LanguageLayout),
                                reinterpret_cast<void*>(&Rows<P>), 0, 0);
}
void* Factory(void* out, void* arg, void* content, const char16_t* title) {
    using Create = void* (GUEST_ABI *)(void*, void*, void*);
    alignas(16) u8 function[FunctionSize] = {};
    *reinterpret_cast<u64*>(function) = image_base + ContentVtable;
    *reinterpret_cast<void**>(function + 8) = content;
    *reinterpret_cast<void**>(function + 0x20) = function;
    // The page is built later (the game's menu task): its layout title is looked up right before
    // the content function runs, which takes the override back.
    title_override = title;
    Game<Create>(CreatePage)(out, arg, function);
    DestroyFunction(function);
    return out;
}
template <int P>
GUEST_ABI void* PageFactory(void* out, void* arg) {
    return Factory(out, arg, reinterpret_cast<void*>(&Content<P>), TextOf(pages[P].name));
}
constexpr std::size_t MaxPages = 6;
void* const factories[MaxPages] = {
    reinterpret_cast<void*>(&PageFactory<0>), reinterpret_cast<void*>(&PageFactory<1>),
    reinterpret_cast<void*>(&PageFactory<2>), reinterpret_cast<void*>(&PageFactory<3>),
    reinterpret_cast<void*>(&PageFactory<4>), reinterpret_cast<void*>(&PageFactory<5>)};

constexpr u32 SystemItem = 101001; ///< "System" in the pause menu's first row: "Saves" follows it
std::atomic<const void*> system_text{nullptr};
constexpr u32 ItemNamesCategory = 10; ///< item names: their repository and table give place names
std::atomic<void*> item_repository{nullptr};
std::atomic<u32> item_table{0};
// Place names for other threads (the overlay): asked for there, looked up on a game thread as it
// looks up its own texts (LookupHook), kept as UTF-8.
std::mutex place_mutex;
std::map<u32, std::string> place_names;
std::vector<u32> places_wanted;
std::atomic<bool> places_pending{false};
std::string Utf8(const char16_t* text) {
    std::string out;
    for (const char16_t* c = text; c && *c; ++c) {
        u32 v = *c;
        if (v >= 0xd800 && v < 0xdc00 && c[1] >= 0xdc00 && c[1] < 0xe000) {
            v = 0x10000 + ((v - 0xd800) << 10) + (c[1] - 0xdc00);
            ++c;
        }
        if (v < 0x80) {
            out += char(v);
        } else if (v < 0x800) {
            out += char(0xc0 | v >> 6), out += char(0x80 | (v & 0x3f));
        } else if (v < 0x10000) {
            out += char(0xe0 | v >> 12), out += char(0x80 | (v >> 6 & 0x3f)), out += char(0x80 | (v & 0x3f));
        } else {
            out += char(0xf0 | v >> 18), out += char(0x80 | (v >> 12 & 0x3f)), out += char(0x80 | (v >> 6 & 0x3f)),
                out += char(0x80 | (v & 0x3f));
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}
constexpr u32 ListTitle = 110020; ///< System's list title ("System"), looked up as its builder runs
std::atomic<const char16_t*> list_title{nullptr}; ///< our list's title, until System's is next looked up
std::atomic<std::chrono::steady_clock::rep> list_title_time{0}; ///< when ItemList set it

// ---- Hooks ----
using LookupFn = const char16_t* (GUEST_ABI *)(void*, u32, u32, u32);
using AddFn = void* (GUEST_ABI *)(void*, void*, void*, void*);
using AddActionFn = void* (GUEST_ABI *)(void*, void*, void*, u64);
LookupFn lookup_original = nullptr;
AddFn add_original = nullptr;
AddActionFn action_original = nullptr;
using AddBlankFn = void* (GUEST_ABI *)(void*);
AddBlankFn blank_original = nullptr;
thread_local int blanks_taken = 0; ///< empty cells "Saves" took the place of, still to drop
std::atomic<const void*> screen_sound_text{nullptr};

GUEST_ABI const char16_t* LookupHook(void* repository, u32 table, u32 category, u32 id) {
    if (id >= IdBase && id < IdBase + texts.size()) {
        return TextOf(id);
    }
    if (title_override && category == TextName && id == (ControlsLayoutUsed() ? ControlsTitle : LanguageTitle)) {
        return title_override;
    }
    const char16_t* text = lookup_original(repository, table, category, id);
    if (category == ItemNamesCategory && !item_repository.load()) {
        item_table = table; // the item texts' table: place names are in it too
        item_repository = repository;
    }
    if (places_pending.exchange(false) && item_repository.load()) {
        std::scoped_lock lock{place_mutex};
        for (const u32 place : places_wanted) {
            place_names[place] = Utf8(lookup_original(item_repository.load(), item_table.load(), 19, place));
        }
        places_wanted.clear();
    }
    static const bool trace = [] {
        const char* env = std::getenv("BB_GAME_MENU_TRACE");
        return env && env[0] == '1';
    }();
    if (trace && id >= 110000 && id < 120000) {
        std::printf("Game menu: text %#x/%u%s from %#llx\n", category, id, title_override ? " (our page)" : "",
                    (unsigned long long)(reinterpret_cast<u64>(__builtin_return_address(0)) - image_base));
    }
    if (category == TextName && id == ListTitle) {
        const auto age = std::chrono::steady_clock::now().time_since_epoch().count() - list_title_time.load();
        const char16_t* title = list_title.exchange(nullptr);
        if (title && age < std::chrono::steady_clock::duration(std::chrono::seconds(2)).count()) {
            return title; // our list's title in place of System's (looked up as the list opens)
        }
    }
    if (category == TextName && id == SystemItem && text) {
        system_text = text;
    }
    if (category == TextName && id == ScreenSoundItem && text) {
        screen_sound_text = text;
        bool cyrillic = false;
        for (const char16_t* c = text; *c; ++c) {
            cyrillic = cyrillic || (*c >= 0x400 && *c < 0x500);
        }
        game_language = cyrillic ? GameLanguage::Russian : GameLanguage::English;
    }
    return text;
}

void AddItem(void* menu, u32 name, u32 help, void* factory) {
    using ToFunction = void* (GUEST_ABI *)(void*, void*);
    alignas(16) u8 pair[2 * TextSize] = {};
    MakeTexts(pair, name, help);
    alignas(16) u8 source[FunctionSize] = {};
    *reinterpret_cast<u64*>(source) = image_base + FactoryVtable;
    *reinterpret_cast<void**>(source + 8) = factory;
    *reinterpret_cast<void**>(source + 0x20) = source;
    alignas(16) u8 function[FunctionSize] = {};
    *reinterpret_cast<void**>(function + 0x20) = Game<ToFunction>(FactoryToFunction)(source, function);
    alignas(16) u8 out[16] = {};
    add_original(menu, pair, function, out);
    DestroyFunction(function);
    DestroyTexts(pair);
}

// ---- The pause menu's "Saves" (bbport_save_menu.cpp does the work). ----
// The lists are System's own: its builder runs and, while it does, the items it adds are
// replaced by ours (AddHook, AddActionHook), so a list looks and works as System does.
// A load row loads on a second press, as in the port's overlay: the first one turns its text into
// "Press again".
constexpr u64 SystemList = 0x1bb3ad0;    ///< (out list, arg, 1): System's item list
constexpr u64 ExitGameYes = 0x1c0d050;   ///< "Exit Game"'s yes: back to the title screen
constexpr u64 ExitState = 0x5562878;     ///< pointer to the state ExitGameYes sets (+0x250: requested)
constexpr std::size_t LoadCopies = 15;   ///< the player's copies the Load list shows

u32 saves_name = 0, saves_help = 0, save_name = 0, save_help = 0;
u32 load_name = 0, load_help = 0, own_name = 0, own_help = 0;
u32 last_name = 0, last_help = 0; ///< "Undo the last load": the state before it
std::string last_copy;                                ///< its copy (load_mutex)
u32 copy_names[LoadCopies] = {}, copy_helps[LoadCopies] = {};
std::mutex load_mutex;
std::vector<std::string> load_list; // the copies the Load list shows, in its order

void DefineSaveTexts() {
    saves_name = AddText("Saves", "Сохранения");
    saves_help = AddText("Save a copy of your progress or load one (bbport)",
                         "Сохранить копию прогресса или загрузить её (bbport)");
    save_name = AddText("Save", "Сохранить");
    save_help = AddText("Make a copy of the save as it is now", "Сделать копию сохранения в нынешнем виде");
    load_name = AddText("Load", "Загрузить");
    load_help = AddText("Load the game's own save or one of your copies",
                        "Загрузить собственное сохранение игры или одну из ваших копий");
    own_name = AddText("Game's autosave", "Автосохранение игры");
    own_help = AddText("Back to the game's last own save", "Вернуться к последнему сохранению самой игры");
    last_name = AddText("Undo the last load", "Отменить последнюю загрузку");
    last_help = AddText("", "");
    for (std::size_t i = 0; i < LoadCopies; ++i) {
        copy_names[i] = AddText("", "");
        copy_helps[i] = AddText("", "");
    }
}

// Load rows: the one pressed once (the game's menu task) and its own text meanwhile.
enum Answer : u64 { AnswerOwn, AnswerLast, AnswerCopy0 };
constexpr auto ArmTime = std::chrono::seconds(4);
std::optional<u64> armed;
std::chrono::steady_clock::time_point armed_time{};
u32 armed_text = 0;
Text armed_original;

void Disarm() {
    if (armed) {
        texts[armed_text - IdBase] = armed_original;
        armed.reset();
    }
}
void LoadAnswer(u64 answer) {
    std::string name;
    {
        std::scoped_lock lock{load_mutex};
        if (answer == AnswerLast) {
            name = last_copy;
        } else if (answer >= AnswerCopy0 && answer - AnswerCopy0 < load_list.size()) {
            name = load_list[answer - AnswerCopy0];
        }
    }
    if (answer == AnswerOwn || !name.empty()) {
        BbSaveMenu::Load(name); // empty: the game's own save
    }
}
/// The game calls an item's action twice for one press (the list's decide builds two branches,
/// 0x1be9380). Calls this close to the one before belong to the same press.
constexpr auto SamePress = std::chrono::milliseconds(250);
std::chrono::steady_clock::time_point last_call{};
/// A new press, not a repeated call of the last one (the game's menu task).
bool NewPress() {
    const auto now = std::chrono::steady_clock::now();
    const bool fresh = now - last_call > SamePress;
    last_call = now;
    return fresh;
}

/// After an item's action the list's decide (0x1be9380) goes one of two ways by the screen the
/// action returned: done (a dialog answered yes, or no screen at all) closes the list back to the
/// one before (ListDone: result code max(flags, 2)); cancelled (no) keeps it (input on again).
/// Our rows return no screen, so the list would close; for one we want kept, ListDoneHook takes
/// the cancelled way instead. The action's argument is the list + 0x48 (the call wrapper 0x1b6ee10).
constexpr u64 ListDone = 0x1becb10;     ///< ({vtable, list, flags}): list result code max(flags, 2)
constexpr u64 ListActivate = 0x1b17980; ///< (list, show): its "FadeIn" (1) or "FadeOut" (0) animation
const u8 ListDonePrologue[] = {0x48, 0x8b, 0x47, 0x08, 0x83, 0x7f, 0x10, 0x02};
using ListDoneFn = void (GUEST_ABI *)(void*);
ListDoneFn list_done_original = nullptr;
std::atomic<u8*> keep_list{nullptr}; ///< the list the press keeps open
std::atomic<u8*> leave_list{nullptr}; ///< the list the press leaves for the game (Save)

/// The press keeps its list open (a load row's first press).
void KeepList(void* arg) {
    keep_list.store(static_cast<u8*>(arg) - 0x48);
}
/// The press closes the whole menu, back to the game (Save).
void LeaveList(void* arg) {
    leave_list.store(static_cast<u8*>(arg) - 0x48);
}
GUEST_ABI void ListDoneHook(void* step) {
    u8* list = *reinterpret_cast<u8**>(static_cast<u8*>(step) + 8);
    u8* expected = list;
    if (keep_list.compare_exchange_strong(expected, nullptr)) {
        // Before the item's screen the list faded out (0x1b17980(list, 0): "FadeOut"); it fades in
        // again and its result code stays 0: it goes on taking input.
        Game<void (GUEST_ABI *)(void*, int)>(ListActivate)(list, 1);
        return;
    }
    expected = list;
    if (!leave_list.compare_exchange_strong(expected, nullptr)) {
        list_done_original(step);
        return;
    }
    // As the cancelled way does (0x1beceb0, then 0x1becd90 with the item's flags): with list+0xd21
    // set it closes the whole menu, back to the game.
    list[0xd21] = 1;
    Game<void (GUEST_ABI *)(void*, int)>(ListActivate)(list, 1);
    const u64 flags = *reinterpret_cast<u64*>(static_cast<u8*>(step) + 0x10);
    if (list[0xde4]) {
        using SetResult = void (GUEST_ABI *)(void*, u64);
        (*reinterpret_cast<SetResult**>(list))[8](list, flags);
    } else {
        *reinterpret_cast<u64*>(list + 0x1c0) = flags;
    }
}

/// A load row pressed: the first press arms it, a second one within ArmTime loads.
void* Press(void* out, void* arg, u64 answer, u32 text) {
    const auto now = std::chrono::steady_clock::now();
    if (!NewPress()) {
        // a repeated call of the same press
    } else if (armed == answer && now - armed_time < ArmTime) {
        Disarm();
        LoadAnswer(answer);
    } else {
        Disarm();
        KeepList(arg);
        armed = answer;
        armed_time = now;
        armed_text = text;
        armed_original = texts[text - IdBase];
        Text& t = texts[text - IdBase];
        t.english = Utf16("Press again to load");
        t.russian = Utf16("Нажмите ещё раз");
        BbSaveMenu::Notice("Press again to load", "Нажмите ещё раз, чтобы загрузить");
    }
    *static_cast<void**>(out) = nullptr; // no screen follows: the list stays
    return out;
}

// Items' actions (called by the game's menu code with the item's argument).
/// "Save" saves at once and closes the menu, back to the game.
GUEST_ABI void* SaveAction(void* out, void* arg) {
    if (NewPress()) {
        LeaveList(arg);
        Disarm();
        BbSaveMenu::Save();
    }
    *static_cast<void**>(out) = nullptr;
    return out;
}
GUEST_ABI void* OwnAction(void* out, void* arg) {
    return Press(out, arg, AnswerOwn, own_name);
}
GUEST_ABI void* LastAction(void* out, void* arg) {
    return Press(out, arg, AnswerLast, last_name);
}
template <int I>
GUEST_ABI void* CopyAction(void* out, void* arg) {
    return Press(out, arg, AnswerCopy0 + I, copy_names[I]);
}
template <std::size_t... I>
constexpr std::array<void*, sizeof...(I)> CopyActions(std::index_sequence<I...>) {
    return {reinterpret_cast<void*>(&CopyAction<int(I)>)...};
}
const std::array<void*, LoadCopies> copy_actions = CopyActions(std::make_index_sequence<LoadCopies>{});

void AddAction(void* builder, u32 name, u32 help, void* action) {
    alignas(16) u8 pair[2 * TextSize] = {};
    MakeTexts(pair, name, help);
    alignas(16) u8 function[FunctionSize] = {};
    *reinterpret_cast<u64*>(function) = image_base + FactoryVtable;
    *reinterpret_cast<void**>(function + 8) = action;
    *reinterpret_cast<void**>(function + 0x20) = function;
    action_original(builder, pair, function, 0);
    DestroyFunction(function);
    DestroyTexts(pair);
}

/// System's item list with our items instead of its own: `fill(builder)` adds them when System's
/// builder adds its first item (on this thread, the game's menu task).
/// In game, System's builder removes the last item after some of its own (Language, and Network
/// offline): in place of each of its items a placeholder goes in for it to remove, and one it kept
/// is removed before the next. Items: 0x180 bytes, the list's end at +0x70.
constexpr std::size_t ItemSize = 0x180, ItemsEnd = 0x70;
thread_local void (*list_fill)(void*) = nullptr;
thread_local bool list_filled = false;
thread_local u8* placeholder_end = nullptr;
void* ItemList(void* out, void* arg, void (*fill)(void*), u32 title) {
    using Build = void* (GUEST_ABI *)(void*, void*, int);
    keep_list.store(nullptr); // ones a press left unused
    leave_list.store(nullptr);
    list_fill = fill;
    list_filled = false;
    placeholder_end = nullptr;
    list_title_time = std::chrono::steady_clock::now().time_since_epoch().count();
    list_title = TextOf(title);
    Game<Build>(SystemList)(out, arg, 1);
    list_fill = nullptr;
    return out;
}
/// While ItemList runs: System's item is dropped, ours go in at the first one. True when so.
bool ReplaceItem(void* builder) {
    if (!list_fill) {
        return false;
    }
    u8*& end = *reinterpret_cast<u8**>(static_cast<u8*>(builder) + ItemsEnd);
    auto fill = list_fill;
    list_fill = nullptr; // our own adds go through
    if (!list_filled) {
        list_filled = true;
        fill(builder);
    } else if (placeholder_end && end == placeholder_end) {
        u8* last = end - ItemSize; // kept: removed as the builder removes one
        using Destroy = void (GUEST_ABI *)(void*);
        (*reinterpret_cast<Destroy**>(last))[1](last);
        end -= ItemSize;
    }
    AddAction(builder, save_name, save_help, reinterpret_cast<void*>(&SaveAction)); // placeholder
    placeholder_end = end;
    list_fill = fill;
    return true;
}

/// A place's name as the game shows it (its PlaceName texts, in the game's language), trimmed.
/// Empty until the game has looked up an item text (its table).
constexpr u32 PlaceNames = 19; ///< PlaceName texts (the Load Game screen's locations)
std::u16string PlaceName(u32 place) {
    void* repository = item_repository.load();
    if (!place || !repository) {
        return {};
    }
    const char16_t* text = lookup_original(repository, item_table.load(), PlaceNames, place);
    std::u16string name = text ? text : u"";
    while (!name.empty() && name.back() == u' ') {
        name.pop_back();
    }
    while (!name.empty() && name.front() == u' ') {
        name.erase(name.begin());
    }
    return name;
}
/// A list row's text on one line: the row wraps at spaces (a second line is not shown), so they
/// are no-break spaces, and a long text is cut to RowChars with an ellipsis.
constexpr std::size_t RowChars = 26;
std::u16string OneLine(std::u16string text) {
    if (text.size() > RowChars) {
        text.resize(RowChars - 1);
        while (!text.empty() && text.back() == u' ') {
            text.pop_back();
        }
        text += u"\u2026";
    }
    for (char16_t& c : text) {
        if (c == u' ') {
            c = u'\u00a0';
        }
    }
    return text;
}

/// "Load": the game's own save, then the player's newest copies.
void FillLoadList(void* builder) {
    armed.reset(); // its texts are written anew below
    const auto copies = BbSaveMenu::List(true);
    {
        AddAction(builder, own_name, own_help, reinterpret_cast<void*>(&OwnAction));
        std::scoped_lock lock{load_mutex};
        last_copy.clear();
        for (const auto& c : BbSaveMenu::List()) {
            if (c.manual) {
                continue;
            }
            // The state before the last load: when and where in the help line.
            const std::string when = c.when;
            const std::u16string shown = Utf16(
                (when.size() >= 16 ? when.substr(8, 2) + "." + when.substr(5, 2) + " " + when.substr(11, 5) : when)
                    .c_str());
            const std::u16string place = PlaceName(c.place);
            const std::u16string where = place.empty() ? shown : shown + u"  " + place;
            Text& help = texts[last_help - IdBase];
            help.english = Utf16("Back to ") + where + Utf16(", as it was before the last load");
            help.russian = Utf16("Вернуться в ") + where + Utf16(", как было до последней загрузки");
            AddAction(builder, last_name, last_help, reinterpret_cast<void*>(&LastAction));
            last_copy = c.name;
            break;
        }
        load_list.clear();
        for (const auto& c : copies) {
            if (load_list.size() == LoadCopies) {
                break;
            }
            const std::size_t i = load_list.size();
            // "2026-10-10 14:30:22" -> "10.10 14:30", then where it was (the game's own place name)
            const std::string when = c.when;
            const std::string shown =
                when.size() >= 16 ? when.substr(8, 2) + "." + when.substr(5, 2) + " " + when.substr(11, 5) : when;
            const std::u16string place = PlaceName(c.place);
            std::u16string label = Utf16(shown.c_str());
            if (!place.empty()) {
                label += u"  " + place;
            }
            Text& name = texts[copy_names[i] - IdBase];
            name.english = name.russian = OneLine(label);
            // The help line under the list: the whole place name.
            Text& help = texts[copy_helps[i] - IdBase];
            help.english = place.empty() ? Utf16("Load this copy") : place;
            help.russian = place.empty() ? Utf16("Загрузить эту копию") : place;
            AddAction(builder, copy_names[i], copy_helps[i], copy_actions[i]);
            load_list.push_back(c.name);
        }
    }
}
GUEST_ABI void* LoadList(void* out, void* arg) {
    return ItemList(out, arg, &FillLoadList, load_name);
}
/// "Saves": Save and Load.
void FillSavesList(void* builder) {
    AddAction(builder, save_name, save_help, reinterpret_cast<void*>(&SaveAction));
    AddItem(builder, load_name, load_help, reinterpret_cast<void*>(&LoadList));
}
GUEST_ABI void* SavesList(void* out, void* arg) {
    return ItemList(out, arg, &FillSavesList, saves_name);
}

GUEST_ABI void* AddActionHook(void* menu, void* pair, void* function, u64 flags) {
    if (ReplaceItem(menu)) {
        return menu;
    }
    return action_original(menu, pair, function, flags);
}

// ---- In play or not: a load leaves through "Exit Game", which the game takes only from its
// in-game step. Set elsewhere (a loading screen, say), another part of the game takes it its own way.
using StepFn = void (GUEST_ABI *)(void*, float);
StepFn step_original = nullptr;
std::atomic<std::int64_t> step_time{0}; ///< steady clock, ns: the step's last run
std::atomic<bool> step_settled{false};  ///< then: not leaving the world, no "Exit Game" waiting

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool ExitRequested() {
    const u8* state = *Game<u8**>(ExitState);
    return state && reinterpret_cast<const std::atomic<u32>*>(state + 0x250)->load() != 0;
}
GUEST_ABI void StepHook(void* step, float frame_time) {
    step_original(step, frame_time);
    const u8* s = static_cast<const u8*>(step);
    // A load while dying is fine: the step takes "Exit Game" then as at any other time.
    // +0x153: leaving the world (to the title, or to reload it after a death or a warp);
    // +0x150: "Exit Game" taken, waiting for a save to end.
    const bool settled = !s[0x153] && !s[0x150] && !ExitRequested();
    step_settled.store(settled);
    step_time.store(NowNs());
    BbSaveMenu::GameStep(settled);
}

/// The first row's empty cells: "Saves" takes the place of one, so the rows below stay as they are.
GUEST_ABI void* AddBlankHook(void* menu) {
    if (blanks_taken > 0) {
        --blanks_taken;
        return menu;
    }
    return blank_original(menu);
}

GUEST_ABI void* AddHook(void* menu, void* pair, void* function, void* out) {
    if (ReplaceItem(menu)) {
        return menu;
    }
    void* result = add_original(menu, pair, function, out);
    const void* system = system_text.load();
    if (system && *reinterpret_cast<const void* const*>(pair) == system && runtime_file_top_menu_fixed()) {
        AddItem(menu, saves_name, saves_help, reinterpret_cast<void*>(&SavesList)); // the row's last
        blanks_taken = 1;
    }
    const void* screen_sound = screen_sound_text.load();
    if (screen_sound && *reinterpret_cast<const void* const*>(pair) == screen_sound) {
        for (std::size_t i = 0; i < pages.size() && i < MaxPages; ++i) {
            AddItem(menu, pages[i].name, pages[i].help, factories[i]);
        }
    }
    return result;
}

/// `jmp hook` at the function (via a near thunk); its moved prologue + `jmp` back is the original.
bool Detour(unsigned char* image, u64 va, const u8* prologue, std::size_t length, void* hook,
            void** original, u8*& cursor) {
    u8* site = image + va;
    if (std::memcmp(site, prologue, length) != 0) {
        std::printf("Game menu: unexpected code at %#llx, not hooked\n", (unsigned long long)va);
        return false;
    }
    u8* trampoline = cursor;
    std::memcpy(trampoline, prologue, length);
    trampoline[length] = 0xe9;
    const std::int64_t back = std::int64_t(site + length) - std::int64_t(trampoline + length + 5);
    const std::int32_t back32 = std::int32_t(back);
    std::memcpy(trampoline + length + 1, &back32, 4);
    u8* thunk = trampoline + 32;
    thunk[0] = 0x48;
    thunk[1] = 0xb8; // movabs rax, hook
    std::memcpy(thunk + 2, &hook, 8);
    thunk[10] = 0xff;
    thunk[11] = 0xe0; // jmp rax
    const std::int64_t to = std::int64_t(thunk) - std::int64_t(site + 5);
    if (back != back32 || to != std::int32_t(to)) {
        std::printf("Game menu: stubs out of jump range, not hooked\n");
        return false;
    }
    const std::int32_t to32 = std::int32_t(to);
    site[0] = 0xe9;
    std::memcpy(site + 1, &to32, 4);
    for (std::size_t i = 5; i < length; ++i) {
        site[i] = 0xcc;
    }
    *original = trampoline;
    cursor += 64;
    return true;
}

/// A mapping within +-2 GiB of the image, for rel32 jumps both ways.
u8* MapNear(unsigned char* image, u64 image_size, std::size_t size) {
    const u64 base = reinterpret_cast<u64>(image);
#ifdef _WIN32
    (void)image_size;
    auto* p = static_cast<u8*>(runtime_low_map(size, 3));
    const u64 at = reinterpret_cast<u64>(p);
    const u64 distance = at > base ? at - base : base - at;
    return p && distance < (2000ull << 20) ? p : nullptr;
#else
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {base - k * (24ull << 20), base + image_size + k * (24ull << 20)}) {
            void* p = mmap(reinterpret_cast<void*>(hint), size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                return static_cast<u8*>(p);
            }
        }
    }
    return nullptr;
#endif
}

} // namespace

std::string PlaceName(std::uint32_t place) {
    if (!place) {
        return {};
    }
    std::scoped_lock lock{place_mutex};
    if (const auto it = place_names.find(place); it != place_names.end()) {
        return it->second;
    }
    if (std::find(places_wanted.begin(), places_wanted.end(), place) == places_wanted.end()) {
        places_wanted.push_back(place);
    }
    places_pending = true;
    return {};
}

void ExitToTitle() {
    using Exit = void (GUEST_ABI *)(void*);
    if (image_base) {
        Game<Exit>(ExitGameYes)(nullptr); // reads only the game's globals
    }
}

bool InPlay() {
    return image_base && step_settled.load() && NowNs() - step_time.load() < 300'000'000;
}

bool ExitPending() {
    return image_base && ExitRequested();
}

std::int64_t StepAgeMs() {
    const std::int64_t at = step_time.load();
    return at ? (NowNs() - at) / 1'000'000 : -1;
}

void CancelExitToTitle() {
    if (image_base) {
        // The request ExitGameYes sets: the game's in-game step clears it when it leaves.
        if (u8* state = *Game<u8**>(ExitState)) {
            reinterpret_cast<std::atomic<u32>*>(state + 0x250)->store(0);
        }
    }
}

void PatchImage(unsigned char* image, std::uint64_t size) {
    if (const char* env = std::getenv("BB_GAME_MENU"); env && env[0] == '0') {
        return;
    }
    image_base = reinterpret_cast<u64>(image);
#ifdef _WIN32
    const std::size_t page = 4096;
#else
    const std::size_t page = std::size_t(sysconf(_SC_PAGESIZE));
#endif
    u8* stubs = MapNear(image, size, page);
    if (!stubs) {
        std::printf("Game menu: no memory near the image, the port's pages are off\n");
        return;
    }
    DefineTexts();
    DefineSaveTexts();
    for (const Page& p : pages) {
        for (const Row& r : p.rows) {
            byte_field[r.field] = r.kind != Kind::List;
        }
    }
    static const BbSettings::Values defaults;
    Fill(defaults, default_values);
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        default_values[FirstEffect + e] = BbSettings::Effects[e].default_on;
    }
    for (int f = 0; f < FieldCount; ++f) {
        default_bytes[f] = u8(default_values[f]);
    }
    u8* cursor = stubs;
    // The lookup first: the menu hook only acts once it has seen the Screen/Sound text.
    const bool ok =
        Detour(image, MsgLookup, MsgLookupPrologue, sizeof(MsgLookupPrologue),
               reinterpret_cast<void*>(&LookupHook), reinterpret_cast<void**>(&lookup_original), cursor) &&
        Detour(image, AddPageItem, AddPageItemPrologue, sizeof(AddPageItemPrologue),
               reinterpret_cast<void*>(&AddHook), reinterpret_cast<void**>(&add_original), cursor) &&
        Detour(image, AddActionItem, AddActionItemPrologue, sizeof(AddActionItemPrologue),
               reinterpret_cast<void*>(&AddActionHook), reinterpret_cast<void**>(&action_original), cursor) &&
        Detour(image, AddBlankItem, AddBlankItemPrologue, sizeof(AddBlankItemPrologue),
               reinterpret_cast<void*>(&AddBlankHook), reinterpret_cast<void**>(&blank_original), cursor) &&
        Detour(image, InGameStep, InGameStepPrologue, sizeof(InGameStepPrologue),
               reinterpret_cast<void*>(&StepHook), reinterpret_cast<void**>(&step_original), cursor) &&
        Detour(image, ListDone, ListDonePrologue, sizeof(ListDonePrologue),
               reinterpret_cast<void*>(&ListDoneHook), reinterpret_cast<void**>(&list_done_original), cursor);
#ifdef _WIN32
    DWORD old_protect = 0;
    VirtualProtect(stubs, page, PAGE_EXECUTE_READ, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), stubs, page);
#else
    mprotect(stubs, page, PROT_READ | PROT_EXEC);
#endif
    std::printf("Game menu: %s (%zu pages in System)\n", ok ? "the port's pages are in" : "hooks failed, off",
                pages.size());
}


void Poll() {
    if (!values_valid) {
        return;
    }
    for (int f = 0; f < FieldCount; ++f) {
        if (byte_field[f]) {
            values[f] = bytes[f];
        }
    }
    if (std::memcmp(values, applied, sizeof(values)) == 0) {
        return;
    }
    auto& s = BbSettings::Get();
    s.output_res = std::clamp(values[OutputRes], 0, BbSettings::OutputCount - 1);
    const int upscaler = std::clamp(values[Upscaler], 0, BbSettings::UpscalerCount - 1);
    s.upscaler = UpscalerAvailable(upscaler) ? upscaler : int(BbSettings::UpscalerFsr3);
    s.preset = std::clamp(values[Preset], 0, BbSettings::PresetCount - 1);
    s.sharpen = values[Sharpness] != 0;
    if (values[Sharpness] != 0) {
        s.sharpness = std::clamp(values[Sharpness], 0, SharpnessSteps - 1) * SharpnessStep;
    }
    s.show_fps = values[ShowFps] != 0;
    s.model_lod = LodValues[std::clamp(values[ModelLod], 0, 3)];
    s.memory_model = std::clamp(values[Memory], 0, BbSettings::MemoryModelCount - 1);
    s.mouse_camera = values[MouseCamera] != 0;
    s.mouse_invert_y = values[MouseInvertY] != 0;
    s.mouse_sensitivity = MouseSensitivities[std::clamp(values[MouseSensitivity], 0, 10)];
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        s.effects[e] = values[FirstEffect + e] != 0;
    }
    std::memcpy(applied, values, sizeof(values));
    BbSettings::Save();
}

} // namespace BbGameMenu
