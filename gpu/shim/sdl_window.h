// bbport: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);
    /// bbport: the mouse for the game (runtime_pad.c). Motion (counts) since the last call when
    /// the camera hook is not in (it gets the motion at once), and the buttons held or clicked
    /// meanwhile (SDL_BUTTON_MASK); true while the window holds the mouse.
    bool TakeMouse(float& dx, float& dy, u32& buttons);

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int text_state{};
    std::string text, text_prompt, base_title;
    void UpdateTextTitle();
    /// Relative mouse mode while mouse_camera is on, the window has focus and neither the
    /// settings menu nor the text entry is shown.
    void UpdateMouseCapture();
    bool HandleMouse(const union SDL_Event& event);
    std::mutex mouse_mutex;
    std::atomic<bool> mouse_captured{false};
    /// A click inside the focused game window arms the capture; losing focus disarms it.
    bool mouse_armed{};
    float mouse_dx{}, mouse_dy{};
    u32 mouse_held{}, mouse_clicked{};
    void UpdateCursor();
    u64 last_mouse_motion_ms{}; ///< SDL_GetTicks of the last mouse motion (UpdateCursor)
    bool cursor_hidden{};
    SDL_Window* window{};
    WindowSystemInfo window_info{};
};

} // namespace Frontend
