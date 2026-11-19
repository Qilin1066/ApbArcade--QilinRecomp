#pragma once
// The recomp's own settings menu, over the game (F1, Escape, or Back and
// Start together on a gamepad): the controls now, the graphics options later.
// Dear ImGui (third_party/imgui, MIT) drawn by its Vulkan backend in the
// renderer's overlay pass, with its SDL3 backend for mouse, keys and pads.
// While it's open the game is paused.

#include "gfx.hpp"
#include "input.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

namespace qilin::app {

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
    // Before each present: builds the menu (or the start-up hint), if either is showing.
    void frame();
    [[nodiscard]] bool quit_requested() const noexcept { return quit_; }

private:
    void controls_tab();
    void steering_section();
    void pedal_section();
    void buttons_section();
    void game_tab();
    void save();

    SDL_Window* window_;
    gfx::Renderer& renderer_;
    Input& input_;
    std::string path_;
    bool ok_ = false, open_ = false, quit_ = false, drawing_ = false;
    std::uint32_t format_ = 0;              // the window's VkFormat, kept for ImGui's pipeline
    std::uint64_t hint_until_ = 0;          // SDL ticks (ms): the "F1: settings" hint shows until then
    int listening_button_ = -1;             // which cabinet button is waiting for a press
    bool measuring_ = false;                // measuring the wheel's rotation...
    double measure_from_ = 0;               // ...from this reading
    bool pad_back_ = false, pad_start_ = false;
};

} // namespace qilin::app
