// The controls layer (input.cpp) on SDL's virtual joysticks: a wheel and a
// set of pedals as separate devices, as a Moza or Logitech set shows up.
// Needs SDL3 and no window or real device.

#include "input.hpp"
#include "menu.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace {

int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

SDL_Joystick* attach(const char* name, SDL_JoystickType type, int axes, int buttons) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = static_cast<Uint16>(type);
    desc.naxes = static_cast<Uint16>(axes);
    desc.nbuttons = static_cast<Uint16>(buttons);
    desc.name = name;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    return id ? SDL_OpenJoystick(id) : nullptr;
}

// Pumps SDL as a frame would, then applies the controls.
void frame(qilin::app::Input& input, qilin::sys2::Inputs& in) {
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e)) input.handle(e);
    input.apply(in);
}

}  // namespace

int main() {
    using namespace qilin;
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Joystick* wheel = attach("Test wheel", SDL_JOYSTICK_TYPE_WHEEL, 1, 8);
    SDL_Joystick* pedals = attach("Test pedals", SDL_JOYSTICK_TYPE_THROTTLE, 3, 0);
    CHECK(wheel && pedals);
    SDL_SetJoystickVirtualAxis(pedals, 0, 32767);    // this pedal rests at the top of its axis
    SDL_SetJoystickVirtualAxis(pedals, 1, -32768);
    SDL_SetJoystickVirtualAxis(pedals, 2, -32768);

    auto owner = std::make_unique<app::Input>();   // closed before SDL_Quit
    app::Input& input = *owner;
    sys2::Inputs in;
    frame(input, in);
    CHECK(input.devices().size() == 2);

    // "Find it": turning the wheel picks its axis.
    input.listen(app::Input::Listen::wheel);
    SDL_SetJoystickVirtualAxis(wheel, 0, 9000);
    frame(input, in);
    const auto heard = input.heard();
    CHECK(heard && heard->kind == app::Binding::Kind::axis && heard->index == 0 && heard->name == "Test wheel");
    if (!heard) return 1;

    // Steering with it: a 900-degree wheel, 160 counts a turn. A quarter turn
    // right (0.2 of the axis) over 20 frames is 40 counts on the dial.
    input.settings.steering = app::Steering::wheel;
    input.settings.wheel_axis = *heard;
    input.settings.wheel = {900, 160, 0.92, 6, false};
    input.configure();
    SDL_SetJoystickVirtualAxis(wheel, 0, 0);
    in.leta[0] = 0;
    frame(input, in);
    const std::uint8_t start = in.leta[0];
    for (int i = 1; i <= 20; ++i) {
        SDL_SetJoystickVirtualAxis(wheel, 0, static_cast<Sint16>(32767 * 0.2 * i / 20));
        frame(input, in);
    }
    // (39.998 counts: the axis's whole-number steps; the fraction is carried, not lost)
    const int moved = static_cast<std::uint8_t>(in.leta[0] - start);
    CHECK(moved == 39 || moved == 40);
    // Held at the right lock, it keeps turning: 6 counts a frame.
    SDL_SetJoystickVirtualAxis(wheel, 0, 32767);
    frame(input, in);
    const std::uint8_t at_lock = in.leta[0];
    frame(input, in);
    CHECK(static_cast<std::uint8_t>(in.leta[0] - at_lock) == 6);

    // Setting up the pedal: the axis that moves furthest, its rest and floor.
    SDL_SetJoystickVirtualAxis(wheel, 0, 32767);
    input.listen(app::Input::Listen::pedal);
    frame(input, in);
    for (const Sint16 v : {16000, 0, -32768, 0, 32767}) {   // pressed to the floor and back up
        SDL_SetJoystickVirtualAxis(pedals, 0, v);
        frame(input, in);
    }
    const auto& found = input.pedal_candidate();
    CHECK(found.kind == app::Binding::Kind::axis && found.index == 0 && found.name == "Test pedals");
    const auto [rest, far] = input.pedal_range();
    CHECK(rest > 0.99 && far < -0.99);
    input.settings.pedal_axis = found;
    input.settings.pedal.released = rest;
    input.settings.pedal.floored = far;
    input.stop_listening();
    frame(input, in);
    CHECK(in.adc[1] == 0x3f);                     // released
    SDL_SetJoystickVirtualAxis(pedals, 0, -32768);
    frame(input, in);
    CHECK(in.adc[1] == 0);                        // floored

    // A wheel button bound to button 1 (IN0 bit 7, active low).
    input.listen(app::Input::Listen::button);
    SDL_SetJoystickVirtualButton(wheel, 3, true);
    frame(input, in);
    const auto button = input.heard();
    CHECK(button && button->kind == app::Binding::Kind::button && button->index == 3);
    if (button) input.settings.buttons[0] = *button;
    frame(input, in);
    CHECK((in.in0 & 0x0080) == 0);
    SDL_SetJoystickVirtualButton(wheel, 3, false);
    frame(input, in);
    CHECK((in.in0 & 0x0080) != 0);

    // The settings file keeps all of it.
    const auto path = (std::filesystem::temp_directory_path() / "apb_input_test.ini").string();
    input.settings.display = {gfx::Scaling::stretch, false, gfx::Effect::crt, 0.25f, 0.75f, false, true, true, "Monitor = 2", "Some GPU"};
    CHECK(input.settings.save(path));
    app::Settings loaded;
    CHECK(loaded.load(path));
    CHECK(loaded.steering == app::Steering::wheel && loaded.wheel_axis.text() == input.settings.wheel_axis.text());
    CHECK(loaded.wheel_axis.name == "Test wheel");
    CHECK(loaded.pedal_axis.text() == input.settings.pedal_axis.text() && std::abs(loaded.pedal.floored - far) < 1e-6);
    CHECK(loaded.buttons[0].text() == input.settings.buttons[0].text());
    CHECK(loaded.display.scaling == gfx::Scaling::stretch && !loaded.display.smooth && !loaded.display.vsync && loaded.display.fullscreen);
    CHECK(loaded.display.match_display);
    CHECK(loaded.display.monitor == "Monitor = 2" && loaded.display.gpu == "Some GPU");
    CHECK(loaded.display.effect == gfx::Effect::crt && loaded.display.scanlines == 0.25f && loaded.display.mask == 0.75f);
    CHECK(app::Settings{}.display.scaling == gfx::Scaling::integer && app::Settings{}.display.vsync);
    // Matching the display: 60.00 Hz is 0.16% from the game's 60.1, so it locks
    // once a refresh; 120 and 240 Hz twice and 4 times; 59.94 still locks;
    // 50, 75, 144 and 165 Hz don't (the exact speed is kept); unknown neither.
    CHECK(app::repeats_for(60.0) == 1 && app::repeats_for(59.94) == 1 && app::repeats_for(60.1) == 1);
    CHECK(app::repeats_for(120.0) == 2 && app::repeats_for(180.0) == 3 && app::repeats_for(240.0) == 4);
    CHECK(app::repeats_for(50.0) == 0 && app::repeats_for(75.0) == 0 && app::repeats_for(144.0) == 0 && app::repeats_for(165.0) == 0);
    CHECK(app::repeats_for(0.0) == 0 && app::repeats_for(300.0) == 0);
    CHECK(app::Settings{}.display.match_display == false);
    // Presets: the defaults are Original; each preset matches itself after it's applied, and
    // leaves full screen, vsync and the display alone; a slider moved by hand is no preset.
    {
        app::DisplaySettings d;
        CHECK(app::matching_preset(d) == 0);
        d.fullscreen = true;
        d.vsync = false;
        d.monitor = "Second";
        for (std::size_t i = 0; i < app::display_presets.size(); ++i) {
            app::apply_preset(d, i);
            CHECK(app::matching_preset(d) == static_cast<int>(i));
            CHECK(d.fullscreen && !d.vsync && d.monitor == "Second");
        }
        app::apply_preset(d, 1);   // the CRT
        CHECK(d.effect == gfx::Effect::crt);
        d.scanlines = 0.5f;
        CHECK(app::matching_preset(d) == -1);
        app::apply_preset(d, 1);
        CHECK(d.scanlines == app::DisplaySettings{}.scanlines && app::matching_preset(d) == 1);
    }
    std::filesystem::remove(path);

    // While the menu is open, the cabinet's controls rest.
    input.playing = false;
    SDL_SetJoystickVirtualButton(wheel, 3, true);
    frame(input, in);
    CHECK((in.in0 & 0x0080) != 0 && in.adc[1] == 0x3f);

    // A gamepad plugged in while running (an Xbox controller, say), with the
    // default settings: A is button 1, Back a coin, the right trigger the
    // gas, the left stick the steering, with nothing set up.
    input.settings = app::Settings{};
    input.configure();
    input.playing = true;
    SDL_Joystick* pad = attach("Test gamepad", SDL_JOYSTICK_TYPE_GAMEPAD, SDL_GAMEPAD_AXIS_COUNT, SDL_GAMEPAD_BUTTON_COUNT);
    CHECK(pad != nullptr);
    frame(input, in);
    CHECK(input.devices().size() == 3 && input.devices().back().gamepad != nullptr);
    frame(input, in);
    CHECK((in.in0 & 0x0080) != 0 && (in.in1 & 0x40) != 0);
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_BACK, true);
    frame(input, in);
    CHECK((in.in0 & 0x0080) == 0 && (in.in1 & 0x40) == 0);
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_BACK, false);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    const std::uint8_t dial = in.leta[0];
    for (int i = 0; i < 10; ++i) frame(input, in);
    CHECK((in.in0 & 0x0080) != 0 && in.adc[1] == 0);   // the gas floored (APB's ADC reads 0 there)
    CHECK(static_cast<std::uint8_t>(in.leta[0] - dial) > 20);   // turning right
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(pad));
    SDL_CloseJoystick(pad);
    frame(input, in);
    CHECK(input.devices().size() == 2);

    owner.reset();
    SDL_CloseJoystick(wheel);
    SDL_CloseJoystick(pedals);
    SDL_Quit();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("input: virtual wheel and pedals, bindings and settings all as expected\n");
    return 0;
}
