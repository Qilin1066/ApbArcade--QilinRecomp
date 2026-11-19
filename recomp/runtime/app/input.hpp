#pragma once
// The cabinet's controls from the player's devices: the keyboard, gamepads,
// wheels, pedals and mice (a spinner is one), with bindings the settings
// menu makes and a settings file keeps.
//
// SDL reads every joystick as raw axes, buttons and hats (a wheel and its
// pedals are often two USB devices, and few are known to SDL's gamepad
// mappings), and gamepads also through SDL's gamepad layer for the defaults.
// A binding names a device by its GUID, so it survives replugging.

#include "controls.hpp"
#include "gfx.hpp"
#include "system2_machine.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qilin::app {

// Where a control comes from.
struct Binding {
    enum class Kind { none, key, button, axis, hat };
    Kind kind = Kind::none;
    std::string device;   // the joystick's GUID; empty for the keyboard
    std::string name;     // the device's name when bound, to show
    int index = 0;        // the scancode, or the button, axis or hat number
    int value = 0;        // an axis as a button: +1 or -1; a hat: its SDL_HAT_* direction

    [[nodiscard]] bool empty() const noexcept { return kind == Kind::none; }
    [[nodiscard]] std::string text() const;              // for the settings file
    static Binding parse(std::string_view text);
    [[nodiscard]] std::string describe() const;          // for the menu
};

// The buttons the cabinet has (MAME's apb inputs), in the order the menu lists them.
enum class Button { one, two, three, coin_left, coin_right, service, count };
inline constexpr std::array<const char*, 6> button_names{"Button 1", "Button 2", "Button 3",
                                                         "Coin, left slot", "Coin, right slot", "Service coin"};
inline constexpr std::array<const char*, 6> button_keys{"button1", "button2", "button3", "coin_left", "coin_right", "service"};

enum class Steering { keys_and_pad, wheel, mouse };

// How the game is shown (the menu's Display tab).
struct DisplaySettings {
    gfx::Scaling scaling = gfx::Scaling::integer;
    bool smooth = true;         // bilinear at scales that aren't whole (with no effect)
    gfx::Effect effect = gfx::Effect::none;
    float scanlines = 0.75f;    // the CRT effect's
    float mask = 0.3f;
    bool vsync = true;
    bool match_display = false; // with vsync: the game locked to a display near 60 Hz (or a multiple), not the cabinet's exact 60.1
    bool fullscreen = false;    // borderless, at the display's own mode
    std::string monitor;        // the display's name; empty: the primary display
    std::string gpu;            // the Vulkan device's name; empty: the best one (used at start-up)
};

// One-click settings for the picture (the Display page's presets): its size
// and look only, never full screen, the display, vsync or the device.
struct DisplayPreset {
    const char* name;
    const char* about;
    gfx::Scaling scaling;
    gfx::Effect effect;
    bool smooth;
};
inline constexpr std::array<DisplayPreset, 3> display_presets{{
    {"Original", "The arcade's picture exactly: whole multiples, plain pixels (the default).", gfx::Scaling::integer, gfx::Effect::none, true},
    {"Arcade monitor", "As big as fits, with the look of the cabinet's CRT.", gfx::Scaling::fit, gfx::Effect::crt, false},
    {"Smooth", "As big as fits, with pixel art's steps smoothed into lines.", gfx::Scaling::fit, gfx::Effect::pixel_art, false},
}};
// Sets the picture to a preset (the CRT's sliders back to their defaults).
void apply_preset(DisplaySettings& d, std::size_t preset);
// The preset the picture is set to now, or -1 (set by hand).
[[nodiscard]] int matching_preset(const DisplaySettings& d);

struct Settings {
    std::array<Binding, 6> buttons;    // each besides the built-in keys and gamepad buttons
    Steering steering = Steering::keys_and_pad;
    Binding wheel_axis;
    controls::WheelSettings wheel;
    Binding pedal_axis;
    controls::PedalSettings pedal;
    double mouse_counts = 0.25;        // dial counts per pixel the mouse (or spinner) moves
    double stick_counts = 6;           // dial counts a frame, stick or keys at full
    int friction = 20;                 // force feedback on the bound wheel, percent; 0 off
    DisplaySettings display;
    std::string rom_dir;               // where the ROMs were last found (apb with no folder given uses it)

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

class Input {
public:
    Input();
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // Every SDL event goes here first (devices coming and going, the mouse, listening).
    void handle(const SDL_Event& e);
    // Once a frame, at vblank: the board's inputs from the devices.
    void apply(sys2::Inputs& in);
    // The settings changed (the menu): rebuilds what depends on them.
    void configure();

    Settings settings;
    bool test_switch = false;
    bool playing = true;    // false while the menu is open: the mouse is the menu's then

    struct Device {
        SDL_JoystickID id = 0;
        SDL_Joystick* joystick = nullptr;
        SDL_Gamepad* gamepad = nullptr;     // also a gamepad SDL knows
        SDL_Haptic* haptic = nullptr;       // force feedback, if it has it
        int effect = -1;                    // the friction (or damper) effect running on it
        std::string guid, name;
        SDL_JoystickType type = SDL_JOYSTICK_TYPE_UNKNOWN;
    };
    [[nodiscard]] const std::vector<Device>& devices() const noexcept { return devices_; }
    [[nodiscard]] static const char* type_name(SDL_JoystickType type);

    // A binding's current reading: an axis -1..1, a button or key 0 or 1.
    [[nodiscard]] double read(const Binding& b) const;

    // Listening for the player to show which control they mean.
    enum class Listen { none, button, wheel, pedal };
    void listen(Listen what);
    [[nodiscard]] Listen listening() const noexcept { return listen_; }
    std::optional<Binding> heard();   // once, when it has heard one
    // While listening for a pedal: the axis that has moved most so far, and
    // its released and furthest readings (the menu's Done button takes them).
    [[nodiscard]] const Binding& pedal_candidate() const noexcept { return pedal_candidate_; }
    [[nodiscard]] std::pair<double, double> pedal_range() const noexcept { return {pedal_rest_, pedal_far_}; }
    void stop_listening() { listen_ = Listen::none; }

    // For the menu's meters: the last frame's dial counts and pedal travel.
    int last_counts = 0;
    double last_pedal = 0;

private:
    Device* find(const std::string& guid);
    const Device* find(const std::string& guid) const;
    void add(SDL_JoystickID id);
    void remove(SDL_JoystickID id);
    void start_force(Device& d);
    void baseline();

    std::vector<Device> devices_;
    controls::Wheel wheel_;
    controls::Rate stick_;
    double mouse_dx_ = 0, mouse_carry_ = 0;
    int pedal_ = 0;   // the keys' and trigger's pedal, 0..0x3f, moved 8 a frame

    Listen listen_ = Listen::none;
    std::optional<Binding> heard_;
    struct Rest {
        std::string guid, name;
        std::vector<double> axes;
    };
    std::vector<Rest> rest_;   // every axis as it was when listening began
    Binding pedal_candidate_;
    double pedal_rest_ = 0, pedal_far_ = 0;
};

} // namespace qilin::app
