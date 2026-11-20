// The controls layer (input.cpp) on SDL's virtual joysticks: a wheel and a
// set of pedals as separate devices, as a Moza or Logitech set shows up.
// Needs SDL3 and no window or real device.

#include "input.hpp"
#include "audio_output.hpp"
#include "menu.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>

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
void pump(qilin::app::Input& input) {
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e)) input.handle(e);
}
void frame(qilin::app::Input& input, qilin::sys2::Inputs& in) {
    pump(input);
    input.apply(in);
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
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

    // A missing haptic driver must not prevent steering. Where the driver
    // can initialise, Input must own its own balanced subsystem reference.
    const bool can_init_haptics = SDL_InitSubSystem(SDL_INIT_HAPTIC);
    if (can_init_haptics) SDL_QuitSubSystem(SDL_INIT_HAPTIC);
    auto owner = std::make_unique<app::Input>();   // closed before SDL_Quit
    app::Input& input = *owner;
    if (can_init_haptics) CHECK((SDL_WasInit(SDL_INIT_HAPTIC) & SDL_INIT_HAPTIC) != 0);
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

    // A wrapping wheel makes three turns in a single game frame, ending
    // at its initial axis position. Sampling only that position loses them.
    input.settings.wheel = {360, 160, 0.9, 6, false, true};
    SDL_SetJoystickVirtualAxis(wheel, 0, 0);
    frame(input, in);
    input.configure();
    for (int i = 1; i <= 48; ++i) {
        double axis = std::fmod(i / 8.0, 2.0);
        if (axis >= 1) axis -= 2;
        SDL_SetJoystickVirtualAxis(wheel, 0, static_cast<Sint16>(axis * (axis < 0 ? 32768 : 32767)));
        pump(input);   // several device reports, no cabinet frame yet
    }
    input.apply(in);
    CHECK(input.last_counts >= 479 && input.last_counts <= 480);
    frame(input, in);
    CHECK(input.last_counts == 0);

    // Calibration continues through the seam while paused; resuming must
    // not transfer any of those menu movements to the cabinet's dial.
    input.set_playing(false);
    input.begin_wheel_measurement();
    for (int i = 1; i <= 16; ++i) {
        double axis = std::fmod(i / 8.0, 2.0);
        if (axis >= 1) axis -= 2;
        SDL_SetJoystickVirtualAxis(wheel, 0, static_cast<Sint16>(axis * (axis < 0 ? 32768 : 32767)));
        pump(input);
    }
    input.apply(in);
    CHECK(std::abs(input.measured_wheel_travel() - 2.0) < 1e-6);
    CHECK(input.last_counts == 0);
    input.end_wheel_measurement();
    // Events already queued during the pause must be discarded on resume,
    // including a full turn that ends on the same reading.
    for (int i = 1; i <= 16; ++i) {
        double axis = std::fmod(i / 8.0, 2.0);
        if (axis >= 1) axis -= 2;
        SDL_SetJoystickVirtualAxis(wheel, 0, static_cast<Sint16>(axis * (axis < 0 ? 32768 : 32767)));
        SDL_UpdateJoysticks();   // queue reports, deliberately do not handle them yet
    }
    input.set_playing(true);
    frame(input, in);
    CHECK(input.last_counts == 0);
    input.settings.wheel = {900, 160, 0.92, 6, false};
    input.configure();
    CHECK(!input.devices().front().force_status.empty());

    // Setting up the pedal: the axis that moves furthest, its rest and floor.
    SDL_SetJoystickVirtualAxis(wheel, 0, 32767);
    frame(input, in);   // settle the wheel before taking the pedal's baseline
    input.listen(app::Input::Listen::pedal);
    frame(input, in);
    for (const Sint16 v : std::array<Sint16, 5>{16000, 0, -32768, 0, 32767}) {   // pressed to the floor and back up
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

    // A wheel button bound to siren/start (APB IN0 bit 3, active low).
    input.listen(app::Input::Listen::button);
    SDL_SetJoystickVirtualButton(wheel, 3, true);
    frame(input, in);
    const auto button = input.heard();
    CHECK(button && button->kind == app::Binding::Kind::button && button->index == 3);
    if (button) input.settings.buttons[0] = *button;
    frame(input, in);
    CHECK((in.in0 & 0x0088) == 0x0080);   // siren low, the unused Paperboy pin stays high
    SDL_SetJoystickVirtualButton(wheel, 3, false);
    frame(input, in);
    CHECK((in.in0 & 0x0008) != 0);

    // The settings file keeps all of it.
    const auto path = (std::filesystem::temp_directory_path() / "apb_input_test.ini").string();
    input.settings.display = {gfx::Scaling::stretch, false, gfx::Effect::crt, 0.25f, 0.75f, false, true, true, "Monitor = 2", "Some GPU"};
    input.settings.rom_dir = "D:\\APB Arcade Project";
    input.settings.wheel.wrap = true;
    input.settings.moza_angle = true;
    input.settings.moza_sdk = "D:\\Wheel tools\\MOZA_SDK.dll";
    CHECK(input.settings.save(path));
    app::Settings loaded;
    CHECK(loaded.load(path));
    CHECK(loaded.steering == app::Steering::wheel && loaded.wheel_axis.text() == input.settings.wheel_axis.text());
    CHECK(loaded.wheel_axis.name == "Test wheel");
    CHECK(loaded.wheel.wrap && loaded.moza_angle && loaded.moza_sdk == input.settings.moza_sdk);
    CHECK(loaded.pedal_axis.text() == input.settings.pedal_axis.text() && std::abs(loaded.pedal.floored - far) < 1e-6);
    CHECK(loaded.buttons[0].text() == input.settings.buttons[0].text());
    CHECK(loaded.display.scaling == gfx::Scaling::stretch && !loaded.display.smooth && !loaded.display.vsync && loaded.display.fullscreen);
    CHECK(loaded.display.match_display);
    CHECK(loaded.rom_dir == "D:\\APB Arcade Project");
    // The stick's settings; a file from before them, with the old stick_counts = 6, gets the new defaults.
    CHECK(loaded.stick_speed == input.settings.stick_speed && loaded.stick_curve == input.settings.stick_curve);
    {
        std::ofstream old_file(path, std::ios::trunc);
        old_file << "stick_counts = 6\n";
    }
    app::Settings from_old;
    CHECK(from_old.load(path) && from_old.stick_speed == 3 && from_old.stick_curve == 2);
    // A damaged or hand-edited settings file must never supply invalid
    // scancodes or non-finite values to the controls' arithmetic.
    for (const char* bad : {"key:-1", "key:999999", "key:2147483648", "key:nan", "key:4junk", "key:4.5",
                            "button:device:-1", "button::0", "axis:device:0:2", "hat:device:0:0"}) {
        CHECK(app::Binding::parse(bad).empty());
    }
    CHECK(app::Binding::parse("key:4").index == 4);
    CHECK(app::Binding::parse("axis:device:0:-1").value == -1);
    CHECK(app::Binding::parse("hat:device:0:8").value == SDL_HAT_LEFT);
    CHECK(input.read({app::Binding::Kind::key, {}, {}, -1, 0}) == 0);
    CHECK(input.read({app::Binding::Kind::key, {}, {}, SDL_SCANCODE_COUNT, 0}) == 0);
    {
        std::ofstream damaged(path, std::ios::trunc);
        damaged << "wheel_rotation = nan\nwheel_counts_per_turn = inf\nwheel_edge = 0.9junk\n"
                   "mouse_counts = 1e9999\nstick_speed = 5junk\nstick_curve = nan\nfriction = nan\n"
                   "pedal_released = -inf\npedal_floored = nan\ncrt_scanlines = nan\ncrt_mask = inf\n"
                   "button1 = key:-1\n";
    }
    app::Settings recovered;
    const app::Settings defaults;
    CHECK(recovered.load(path));
    CHECK(recovered.buttons[0].empty());
    CHECK(recovered.wheel.rotation == defaults.wheel.rotation);
    CHECK(recovered.wheel.counts_per_turn == defaults.wheel.counts_per_turn && recovered.wheel.edge == defaults.wheel.edge);
    CHECK(recovered.mouse_counts == defaults.mouse_counts && recovered.stick_speed == defaults.stick_speed);
    CHECK(recovered.stick_curve == defaults.stick_curve && recovered.friction == defaults.friction);
    CHECK(recovered.pedal.released == defaults.pedal.released && recovered.pedal.floored == defaults.pedal.floored);
    CHECK(recovered.display.scanlines == defaults.display.scanlines && recovered.display.mask == defaults.display.mask);
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
    CHECK((in.in0 & 0x0008) != 0 && in.adc[1] == 0x3f);

    // A gamepad plugged in while running (an Xbox controller, say), with the
    // default settings: A is siren/start, Back a coin, the right trigger the
    // gas, the left stick the steering, with nothing set up.
    input.settings = app::Settings{};
    input.configure();
    input.playing = true;
    SDL_Joystick* pad = attach("Test gamepad", SDL_JOYSTICK_TYPE_GAMEPAD, SDL_GAMEPAD_AXIS_COUNT, SDL_GAMEPAD_BUTTON_COUNT);
    CHECK(pad != nullptr);
    frame(input, in);
    CHECK(input.devices().size() == 3 && input.devices().back().gamepad != nullptr);
    frame(input, in);
    CHECK((in.in0 & 0x0008) != 0 && (in.in1 & 0x40) != 0);
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_BACK, true);
    frame(input, in);
    CHECK((in.in0 & 0x008a) == 0x0082 && (in.in1 & 0x40) == 0);
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_BACK, false);
    // B fires on APB's bit 1; X remains the alternate siren on bit 3.
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_EAST, true);
    frame(input, in);
    CHECK((in.in0 & 0x008a) == 0x0088);
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_WEST, true);
    frame(input, in);
    CHECK((in.in0 & 0x008a) == 0x0082);
    SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_WEST, false);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    const std::uint8_t dial = in.leta[0];
    for (int i = 0; i < 10; ++i) frame(input, in);
    CHECK((in.in0 & 0x0008) != 0 && in.adc[1] == 0);   // the gas floored (APB's ADC reads 0 there)
    CHECK(static_cast<std::uint8_t>(in.leta[0] - dial) == 30);   // turning right: 3 counts a frame, fully over
    // Half way over: (0.5 - 0.15) / 0.85 squared, 17% of full speed: 5 counts in 10 frames, not 15.
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFTX, 16384);
    frame(input, in);
    const std::uint8_t half = in.leta[0];
    for (int i = 0; i < 10; ++i) frame(input, in);
    CHECK(static_cast<std::uint8_t>(in.leta[0] - half) >= 4 && static_cast<std::uint8_t>(in.leta[0] - half) <= 6);
    // Inside the dead zone: still.
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFTX, 4000);
    frame(input, in);
    const std::uint8_t still = in.leta[0];
    for (int i = 0; i < 10; ++i) frame(input, in);
    CHECK(in.leta[0] == still);
    SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(pad));
    SDL_CloseJoystick(pad);
    frame(input, in);
    CHECK(input.devices().size() == 2);

#ifdef SDL_PLATFORM_WINDOWS
    // Exercise the official SDK's Win64 export names on a test DLL, with
    // no proprietary files or physical wheel in CI.
    CHECK(argc == 2);
    if (argc == 2) {
        SDL_SharedObject* mock = SDL_LoadObject(argv[1]);
        CHECK(mock != nullptr);
        if (mock) {
            const auto set_angle = reinterpret_cast<void (*)(float)>(SDL_LoadFunction(mock, "mock_set_angle"));
            const auto set_error = reinterpret_cast<void (*)(int)>(SDL_LoadFunction(mock, "mock_set_error"));
            const auto installs = reinterpret_cast<int (*)()>(SDL_LoadFunction(mock, "mock_installs"));
            const auto removes = reinterpret_cast<int (*)()>(SDL_LoadFunction(mock, "mock_removes"));
            CHECK(set_angle && set_error && installs && removes);
            if (set_angle && set_error && installs && removes) {
                input.settings.steering = app::Steering::wheel;
                input.settings.wheel_axis = *heard;
                input.settings.moza_angle = true;
                input.settings.moza_sdk = argv[1];
                set_angle(72000);
                input.configure();
                frame(input, in);
                if (installs() != 1 || input.last_counts != 0)
                    std::fprintf(stderr, "MOZA mock connection: %s; installs=%d, counts=%d\n", input.moza_status().c_str(), installs(), input.last_counts);
                CHECK(installs() == 1 && input.last_counts == 0);
                set_angle(72360);
                frame(input, in);
                CHECK(input.moza_degrees() == 72360 && input.last_counts == 160);
                set_error(5);   // lost intermediate packets, current absolute angle is valid
                set_angle(72720);
                frame(input, in);
                CHECK(input.moza_degrees() == 72720 && input.last_counts == 160);
                set_error(2);   // disconnected: never reuse an old reading
                frame(input, in);
                CHECK(!input.moza_degrees() && input.last_counts == 0);
                set_error(0);
                set_angle(-72000);
                frame(input, in);
                CHECK(input.last_counts == 0);   // reconnect rebases
                set_angle(-71640);
                frame(input, in);
                CHECK(input.last_counts == 160);
                set_angle(std::numeric_limits<float>::quiet_NaN());
                frame(input, in);
                CHECK(!input.moza_degrees() && input.last_counts == 0);
                input.settings.moza_angle = false;
                input.configure();
                CHECK(removes() == 1);
            }
            SDL_UnloadObject(mock);
        }
    }
#else
    app::MozaAngle unavailable;
    CHECK(!unavailable.open("MOZA_SDK.dll") && !unavailable.read());
#endif

    // Disconnect and reconnect the same wheel between cabinet frames.
    // The new position must not be interpreted as a steering movement.
    input.settings = app::Settings{};
    input.settings.steering = app::Steering::wheel;
    input.settings.wheel_axis = *heard;
    input.settings.wheel.edge = 1;
    input.configure();
    SDL_SetJoystickVirtualAxis(wheel, 0, 16000);
    frame(input, in);
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(wheel));
    SDL_CloseJoystick(wheel);
    pump(input);
    wheel = attach("Test wheel", SDL_JOYSTICK_TYPE_WHEEL, 1, 8);
    CHECK(wheel != nullptr);
    SDL_SetJoystickVirtualAxis(wheel, 0, -16000);
    frame(input, in);
    CHECK(input.devices().size() == 2);
    CHECK(input.last_counts == 0);

    // Keep independent setups for two wheel GUIDs, including separate
    // pedals/buttons; loading a wheel never changes display or ROM settings.
    input.settings = app::Settings{};
    input.settings.steering = app::Steering::wheel;
    input.settings.wheel_axis = *heard;
    input.settings.wheel.rotation = 900;
    input.settings.wheel.wrap = false;
    input.settings.friction = 17;
    input.configure();
    const std::string first_guid = input.settings.wheel_axis.device;
    SDL_Joystick* other = attach("Another wheel", SDL_JOYSTICK_TYPE_WHEEL, 2, 8);
    CHECK(other != nullptr);
    pump(input);
    std::string other_guid;
    for (const auto& d : input.devices()) if (d.id == SDL_GetJoystickID(other)) other_guid = d.guid;
    CHECK(first_guid != other_guid && other_guid.size() == 32);
    input.settings.wheel_axis.device = other_guid;
    input.settings.wheel.rotation = 360;
    input.settings.wheel.wrap = true;
    input.settings.friction = 31;
    input.settings.pedal_axis.device = other_guid;
    input.settings.pedal_axis.kind = app::Binding::Kind::axis;
    input.settings.pedal_axis.index = 1;
    input.configure();
    CHECK(input.settings.profiles.size() == 2);
    input.settings.display.mask = 0.42f;
    input.settings.rom_dir = "shared-ROM-folder";
    CHECK(input.settings.save(path));
    app::Settings restored;
    CHECK(restored.load(path) && restored.profiles.size() == 2);
    CHECK(restored.select_profile(first_guid));
    CHECK(restored.wheel.rotation == 900 && !restored.wheel.wrap && restored.friction == 17);
    CHECK(restored.display.mask == 0.42f && restored.rom_dir == "shared-ROM-folder");
    CHECK(restored.select_profile(other_guid));
    CHECK(restored.wheel.wrap && restored.friction == 31 && restored.pedal_axis.device == other_guid);
    input.settings.wheel_axis.device = first_guid;
    input.configure();
    CHECK(input.settings.wheel.rotation == 900 && input.settings.friction == 17);
    // Losing focus releases controls; returning rebases motion. Opening a
    // menu while unfocused must not accidentally reactivate the controls.
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    input.handle(focus);
    CHECK(!input.playing && !input.focused());
    SDL_SetJoystickVirtualAxis(wheel, 0, 30000);
    frame(input, in);
    CHECK(input.last_counts == 0 && in.adc[1] == 63);
    input.set_playing(false);
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    input.handle(focus);
    CHECK(!input.playing && input.focused());
    input.set_playing(true);
    frame(input, in);
    CHECK(input.last_counts == 0);
    // Disconnecting the selected wheel restores the other connected setup.
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(wheel));
    pump(input);
    CHECK(input.settings.wheel_axis.device == other_guid && input.settings.friction == 31);
    SDL_SetJoystickVirtualAxis(other, 0, -32768); frame(input, in);
    SDL_SetJoystickVirtualAxis(other, 0, 32767); frame(input, in);
    const auto diagnostics = input.diagnostics();
    CHECK(diagnostics.find("Another wheel") != std::string::npos);
    CHECK(diagnostics.find("Friction percent: 31") != std::string::npos);
    CHECK(diagnostics.find("observed_min=-32768 observed_max=32767 reached_endpoint=1") != std::string::npos);
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(other));
    SDL_CloseJoystick(other);
    pump(input);
    std::filesystem::remove(path);

    owner.reset();
    CHECK((SDL_WasInit(SDL_INIT_HAPTIC) & SDL_INIT_HAPTIC) == 0);
    SDL_CloseJoystick(wheel);
    SDL_CloseJoystick(pedals);
    // Exercise stream setup, prebuffering, pause and restart without a
    // physical playback device on all platforms.
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    {
        app::AudioOutput output;   // no audio subsystem yet: an initial outage
        CHECK(!output.ok());
        CHECK(SDL_InitSubSystem(SDL_INIT_AUDIO));
        output.request_reopen();
        const std::array<std::int16_t, 1864> pcm{};
        for (int i = 0; i < 20; ++i) CHECK(output.push(pcm));
        CHECK(output.state() == app::AudioOutput::State::playing);
        CHECK(output.pause());
        CHECK(output.state() == app::AudioOutput::State::buffering);
        output.request_reopen();   // the same path as a device reconnect event
        for (int i = 0; i < 3; ++i) CHECK(output.push(pcm));
        CHECK(output.ok() && output.state() == app::AudioOutput::State::playing);
        const auto audio_report = output.diagnostics();
        CHECK(audio_report.find("Audio open attempts: 3\n") != std::string::npos);
        CHECK(audio_report.find("Audio submitted stereo frames: 21436\n") != std::string::npos);
        CHECK(output.pause());
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    SDL_Quit();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("input: virtual wheel and pedals, bindings and settings all as expected\n");
    return 0;
}
