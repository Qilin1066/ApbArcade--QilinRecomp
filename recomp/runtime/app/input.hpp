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
#include "moza.hpp"
#include "gfx.hpp"
#include "system2_machine.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qilin::profile { class Profiler; }
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

// Action bindings in menu/save-file order; the first and third both drive
// APB's siren/start, the second drives fire.
enum class Button { one, two, three, coin_left, coin_right, service, count };
inline constexpr std::array<const char*, 6> button_names{"Siren / start", "Fire", "Siren (alternate)",
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
    bool performance_overlay = false;  // the frame-time readout (F3), as the other recomps have it
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

struct ControlSettings {
    std::array<Binding, 6> buttons;    // each besides the built-in keys and gamepad buttons
    Steering steering = Steering::keys_and_pad;
    Binding wheel_axis;
    controls::WheelSettings wheel;
    bool moza_angle = false;          // physical degrees from the optional Windows MOZA SDK
    std::string moza_sdk;             // empty: MOZA_SDK.dll beside the executable
    Binding pedal_axis;
    controls::PedalSettings pedal;
    double mouse_counts = 0.25;        // dial counts per pixel the mouse (or spinner) moves
    // A gamepad's stick and the arrow keys: dial counts a frame at full, and
    // the stick's response curve (1 straight; higher, finer near the centre).
    // Saved as stick_speed: the old stick_counts (6 at full, straight) was far too
    // quick on a gamepad, so a saved one is left behind.
    double stick_speed = 3;
    double stick_curve = 2;
    int friction = 20;                 // force feedback on the bound wheel, percent; 0 off
};

struct Settings : ControlSettings {
    std::map<std::string, ControlSettings, std::less<>> profiles;
    void remember_controls();
    bool select_profile(const std::string& guid);
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
    profile::Profiler* profiler = nullptr;
    [[nodiscard]] std::string diagnostics() const;
    bool test_switch = false;
    bool playing = true;    // false while the menu is open: the mouse is the menu's then
    void set_playing(bool playing);
    void set_focused(bool focused);
    [[nodiscard]] bool focused() const { return focused_; }

    struct Device {
        SDL_JoystickID id = 0;
        SDL_Joystick* joystick = nullptr;
        SDL_Gamepad* gamepad = nullptr;     // also a gamepad SDL knows
        SDL_Haptic* haptic = nullptr;       // force feedback, if it has it
        int effect = -1;                    // the friction (or damper) effect running on it
        std::string force_status;
        std::string guid, name;
        std::vector<int> axis_min, axis_max;
        SDL_JoystickType type = SDL_JOYSTICK_TYPE_UNKNOWN;
    };
    [[nodiscard]] const std::vector<Device>& devices() const noexcept { return devices_; }
    [[nodiscard]] static const char* type_name(SDL_JoystickType type);

    // A binding's current reading: an axis -1..1, a button or key 0 or 1.
    [[nodiscard]] double read(const Binding& b) const;
    [[nodiscard]] const std::string& moza_status() const { return moza_.status(); }
    [[nodiscard]] std::optional<double> moza_degrees() const { return moza_degrees_; }
    void retry_moza() { moza_.close(); configure(); }
    void begin_wheel_measurement();
    void end_wheel_measurement() { measuring_wheel_ = false; }
    [[nodiscard]] double measured_wheel_travel() const { return measurement_.travel(); }

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
    void reset_motion();
    void baseline();

    std::string active_profile_;
    std::vector<Device> devices_;
    controls::Wheel wheel_;
    controls::Rate stick_;
    MozaAngle moza_;
    std::string moza_path_;
    std::optional<double> moza_degrees_, moza_min_, moza_max_;
    controls::AxisTravel measurement_;
    bool measuring_wheel_ = false;
    bool haptics_ready_ = false;
    bool focused_ = true, requested_playing_ = true;
    bool was_playing_ = true;
    bool resuming_ = false;
    // The gamepad's Back is coin 1, and Back + Start is the settings menu (as
    // in the other recomps): Back counts as a coin once it has been held for
    // a few frames without Start, so opening the menu doesn't drop one in.
    int back_frames_ = 0;
    bool back_with_start_ = false;
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
