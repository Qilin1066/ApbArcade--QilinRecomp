#pragma once
// The recomp's own settings menu, over the game (F1 or Escape, or a
// controller's Back + Start, as in the other recomps): the controls and the
// display. F11 or Alt+Enter switches full screen whether it's open or not;
// F3 shows frame times.
// Dear ImGui (third_party/imgui, MIT) drawn by its Vulkan backend in the
// renderer's overlay pass, with its SDL3 backend for mouse, keys and pads.
// While it's open the game is paused.

#include "gfx.hpp"
#include "input.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace qilin::app {

// The connected display with this name, or 0.
[[nodiscard]] SDL_DisplayID display_named(const std::string& name);

// The board's frame rate: 10 MHz / 166,400 cycles a frame.
inline constexpr double game_hz = 10e6 / 166400.0;
// With "match the display" (and vsync): how many times each game frame is
// shown when the game is locked to the window's display (1 at about 60 Hz,
// 2 at 120, up to 4 at 240), or 0 when it isn't (not asked for, or the
// display's rate isn't within 1% of a whole multiple of the game's: the
// game keeps its exact speed). *refresh gets the display's rate (0 unknown).
// 'vsync_allowed' false (--no-vsync): never locked, whatever is saved.
[[nodiscard]] int display_repeats(SDL_Window* window, const DisplaySettings& d, double* refresh = nullptr, bool vsync_allowed = true);
// The same arithmetic for a known refresh rate (tested).
[[nodiscard]] int repeats_for(double refresh_hz);

class Menu {
public:
    // Fails loudly (returns with ok() false) if the renderer has no window.
    Menu(SDL_Window* window, gfx::Renderer& renderer, Input& input, std::string settings_path);
    ~Menu();
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    void set_open(bool open);
    void toggle() { set_open(!open_); }

    // Every SDL event: the menu's own, and its shortcut keys and buttons.
    void handle(const SDL_Event& e);
    // Before each present: builds the menu (or the start-up hint), if either
    // is showing. 'away': the game is paused because the window isn't the
    // active one; a note in the middle of the picture says so.
    void frame(bool away = false);
    [[nodiscard]] bool quit_requested() const noexcept { return quit_; }
    // Set by apb when locking to the display didn't hold (shown on the Display page).
    std::string speed_note;
    // --no-vsync: vsync is off for this run, whatever the saved setting says.
    bool no_vsync = false;
    // A line shown in the window's corner for a few seconds (a controller coming or going).
    void notify(std::string text);

    // The window to full screen (borderless, on its display) or back.
    void set_fullscreen(bool fullscreen);

private:
    void controls_tab();
    void display_tab();
    void move_to_display(SDL_DisplayID display);
    void steering_section();
    void pedal_section();
    void buttons_section();
    void game_tab();
    void save();
    void draw_frame_times();
    void apply_scale();

    // F3: the last 20 s of intervals between frames shown, in ms.
    static constexpr std::size_t frame_history = 1200;
    std::vector<float> frame_ms_ = std::vector<float>(frame_history, 0.0f);
    std::size_t frame_next_ = 0, frame_count_ = 0;
    std::uint64_t last_frame_ns_ = 0;
    int page_ = 0;                          // the page showing: Controls, Display, Game
    int select_page_ = -1;                  // a page asked for by Page Up/Down or a shoulder button
    bool peek_ = false;                     // "Look at the picture": the menu hidden until any key or button
    bool close_asked_ = false;              // the Close button (acted on after the frame is built)

    SDL_Window* window_;
    gfx::Renderer& renderer_;
    Input& input_;
    std::string path_;
    bool ok_ = false, open_ = false, quit_ = false, drawing_ = false;
    std::uint64_t hint_until_ = 0;          // SDL ticks (ms): the "F1: settings" hint shows until then
    struct Notice {
        std::string text;
        std::uint64_t until;                // SDL ticks (ms)
    };
    std::vector<Notice> notices_;
    int listening_button_ = -1;             // which cabinet button is waiting for a press
    bool finding_wheel_ = false;            // or the wheel is being looked for
    bool measuring_ = false;                // measuring the wheel's rotation...
    float scale_ = 0.0f;                    // the content scale the style is sized for
    bool sdl_backend_ = false;              // ImGui's SDL3 backend is up (shut down even if Vulkan's never was)
};

} // namespace qilin::app
