#include "input.hpp"
#include "path.hpp"
#include "persistence.hpp"
#include "profile.hpp"

#include <cstdio>
#include "text.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <map>
#include <sstream>

namespace qilin::app {

namespace {

// APB overrides Paperboy's button pins: bit 7 is unused, bit 3 is
// siren/start, bit 1 is fire. Keep the third slot as a siren alternative
// so existing Space/C, gamepad X and saved bindings still work.
constexpr std::array<std::uint16_t, 3> in0_bits{0x0008, 0x0002, 0x0008};
constexpr std::array<std::uint8_t, 3> in1_bits{0x40, 0x80, 0x20};          // left coin, right coin, service coin on IN1

double axis_of(SDL_Joystick* j, int axis) {
    const Sint16 v = SDL_GetJoystickAxis(j, axis);
    return v < 0 ? v / 32768.0 : v / 32767.0;
}

std::string guid_of(SDL_JoystickID id) {
    char text[64] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), text, sizeof text);
    return text;
}

double number(std::string_view text, double fallback) {
    const std::string s(text);
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    return end != s.c_str() && end == s.c_str() + s.size() && errno != ERANGE && std::isfinite(v) ? v : fallback;
}

}  // namespace

// ---- Bindings ---------------------------------------------------------------

std::string Binding::text() const {
    switch (kind) {
    case Kind::key: return strf("key:%d", index);
    case Kind::button: return strf("button:%s:%d", device.c_str(), index);
    case Kind::axis: return strf("axis:%s:%d:%d", device.c_str(), index, value);
    case Kind::hat: return strf("hat:%s:%d:%d", device.c_str(), index, value);
    default: return "none";
    }
}

Binding Binding::parse(std::string_view text) {
    Binding b;
    std::vector<std::string_view> parts;
    for (std::size_t at = 0;;) {
        const auto colon = text.find(':', at);
        parts.push_back(text.substr(at, colon == std::string_view::npos ? std::string_view::npos : colon - at));
        if (colon == std::string_view::npos) break;
        at = colon + 1;
    }
    auto num = [&](std::size_t i) -> std::optional<int> {
        if (parts.size() <= i) return std::nullopt;
        int value = 0;
        const auto part = parts[i];
        const auto result = std::from_chars(part.data(), part.data() + part.size(), value);
        if (result.ec != std::errc{} || result.ptr != part.data() + part.size()) return std::nullopt;
        return value;
    };
    if (parts[0] == "key" && parts.size() == 2) {
        const auto index = num(1);
        if (!index || *index <= SDL_SCANCODE_UNKNOWN || *index >= SDL_SCANCODE_COUNT) return {};
        switch (*index) {   // the program's own keys: Escape, F1, F2, F3, F11
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F1: case SDL_SCANCODE_F2: case SDL_SCANCODE_F3: case SDL_SCANCODE_F11: return {};
        default: break;
        }
        b.kind = Kind::key;
        b.index = *index;
    } else if (parts[0] == "button" && parts.size() == 3) {
        const auto index = num(2);
        if (parts[1].empty() || !index || *index < 0) return {};
        b.kind = Kind::button;
        b.device = parts[1];
        b.index = *index;
    } else if ((parts[0] == "axis" || parts[0] == "hat") && parts.size() == 4) {
        const auto index = num(2), value = num(3);
        if (parts[1].empty() || !index || *index < 0 || !value) return {};
        if (parts[0] == "axis") {
            if (*value < -1 || *value > 1) return {};
        } else if (*value != SDL_HAT_UP && *value != SDL_HAT_DOWN && *value != SDL_HAT_LEFT && *value != SDL_HAT_RIGHT) return {};
        b.kind = parts[0] == "axis" ? Kind::axis : Kind::hat;
        b.device = parts[1];
        b.index = *index;
        b.value = *value;
    }
    return b;
}

std::string Binding::describe() const {
    const std::string on = name.empty() ? std::string() : " on " + name;
    switch (kind) {
    case Kind::key: return strf("Key %s", SDL_GetScancodeName(static_cast<SDL_Scancode>(index)));
    case Kind::button: return strf("Button %d%s", index + 1, on.c_str());
    case Kind::axis:
        return value == 0 ? strf("Axis %d%s", index + 1, on.c_str()) : strf("Axis %d %s%s", index + 1, value > 0 ? "+" : "-", on.c_str());
    case Kind::hat: return strf("Hat %d %s%s", index + 1,
                                value == SDL_HAT_UP ? "up" : value == SDL_HAT_DOWN ? "down" : value == SDL_HAT_LEFT ? "left" : "right", on.c_str());
    default: return "(none)";
    }
}

// ---- Settings ---------------------------------------------------------------

void apply_preset(DisplaySettings& d, std::size_t preset) {
    const DisplayPreset& p = display_presets.at(preset);
    const DisplaySettings defaults;
    d.scaling = p.scaling;
    d.effect = p.effect;
    d.smooth = p.smooth;
    d.scanlines = defaults.scanlines;
    d.mask = defaults.mask;
}

int matching_preset(const DisplaySettings& d) {
    const DisplaySettings defaults;
    for (std::size_t i = 0; i < display_presets.size(); ++i) {
        const DisplayPreset& p = display_presets[i];
        // Smoothing only matters with no effect; the sliders only with the CRT.
        if (d.scaling != p.scaling || d.effect != p.effect) continue;
        if (p.effect == gfx::Effect::none && d.smooth != p.smooth) continue;
        if (p.effect == gfx::Effect::crt && (d.scanlines != defaults.scanlines || d.mask != defaults.mask)) continue;
        return static_cast<int>(i);
    }
    return -1;
}

bool Settings::load(const std::string& path) {
    std::ifstream in(qilin::native_path(path));
    if (!in) return false;
    std::map<std::string, std::string, std::less<>> kv;
    bool first_line = true;
    for (std::string line; std::getline(in, line);) {
        if (first_line && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
        first_line = false;
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t\r"));
            s.erase(s.find_last_not_of(" \t\r") + 1);
            return s;
        };
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    if (in.bad()) return false; // never apply a partially read configuration
    std::string prefix;
    auto get = [&](std::string_view key) -> std::string_view {
        const auto it = kv.find(prefix + std::string(key));
        return it == kv.end() ? std::string_view() : std::string_view(it->second);
    };
    auto num = [&](std::string_view key, double fallback) { return get(key).empty() ? fallback : number(get(key), fallback); };
    auto binding = [&](std::string_view key) {
        Binding b = Binding::parse(get(key));
        b.name = std::string(get(std::string(key) + "_name"));
        return b;
    };
    auto controls = [&](ControlSettings& c) {
        for (std::size_t i = 0; i < c.buttons.size(); ++i) c.buttons[i] = binding(button_keys[i]);
        const auto steer = get("steering");
        c.steering = steer == "wheel" ? Steering::wheel : steer == "mouse" ? Steering::mouse : Steering::keys_and_pad;
        c.wheel_axis = binding("wheel_axis");
        c.wheel.rotation = std::clamp(num("wheel_rotation", c.wheel.rotation), 90.0, 10000.0);
        c.wheel.counts_per_turn = std::clamp(num("wheel_counts_per_turn", c.wheel.counts_per_turn), 8.0, 2048.0);
        c.wheel.invert = num("wheel_invert", 0) != 0;
        c.moza_angle = num("moza_angle", 0) != 0;
        // A wheel set up for limited rotation by an older version (it wrote wheel_edge, and
        // wheel_wrap = 1 only for a wrapping axis): a G920 or the like, no longer supported. Its
        // axis read as a wrapping one would steer wrongly, so steering goes back to the keys and
        // gamepad, and its lock-to-lock angle isn't kept as a wrapping axis's.
        if (!get("wheel_edge").empty() && get("wheel_wrap") != "1" && !c.moza_angle) {
            if (c.steering == Steering::wheel) c.steering = Steering::keys_and_pad;
            c.wheel.rotation = controls::WheelSettings{}.rotation;
        }
        c.moza_sdk = std::string(get("moza_sdk"));
        c.pedal_axis = binding("pedal_axis");
        c.pedal.released = std::clamp(num("pedal_released", c.pedal.released), -1.0, 1.0);
        c.pedal.floored = std::clamp(num("pedal_floored", c.pedal.floored), -1.0, 1.0);
        c.pedal.deadzone = std::clamp(num("pedal_deadzone", c.pedal.deadzone), 0.0, 0.3);
        c.mouse_counts = std::clamp(num("mouse_counts", c.mouse_counts), 0.01, 8.0);
        c.stick_speed = std::clamp(num("stick_speed", c.stick_speed), 0.5, 16.0);
        c.stick_curve = std::clamp(num("stick_curve", c.stick_curve), 1.0, 4.0);
        c.friction = static_cast<int>(std::clamp(num("friction", c.friction), 0.0, 100.0));
    };
    controls(*this);
    const auto scaling = get("scaling");
    display.scaling = scaling == "fit" ? gfx::Scaling::fit : scaling == "stretch" ? gfx::Scaling::stretch : gfx::Scaling::integer;
    display.smooth = num("smooth", 1) != 0;
    const auto effect = get("effect");
    display.effect = effect == "pixel_art" ? gfx::Effect::pixel_art : effect == "crt" ? gfx::Effect::crt : gfx::Effect::none;
    display.scanlines = static_cast<float>(std::clamp(num("crt_scanlines", display.scanlines), 0.0, 1.0));
    display.mask = static_cast<float>(std::clamp(num("crt_mask", display.mask), 0.0, 1.0));
    display.vsync = num("vsync", 1) != 0;
    display.match_display = num("match_display", 0) != 0;
    display.fullscreen = num("fullscreen", 0) != 0;
    display.monitor = std::string(get("monitor"));
    display.gpu = std::string(get("gpu"));
    display.performance_overlay = num("performance_overlay", 0) != 0;
    sound.volume = static_cast<int>(std::clamp(num("volume", 100), 0.0, 100.0));
    rom_dir = std::string(get("rom_dir"));
    profiles.clear();
    for (const auto& [key, value] : kv) {
        (void)value;
        if (key.starts_with("profile.") && key.size() > 41 && key[40] == '.') {
            const std::string guid = key.substr(8, 32);
            if (guid.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) profiles.try_emplace(guid);
        }
    }
    for (auto& [guid, profile] : profiles) {
        prefix = "profile." + guid + ".";
        controls(profile);
    }
    std::erase_if(profiles, [](const auto& item) {
        return item.second.steering != Steering::wheel || item.second.wheel_axis.device != item.first;
    });
    return true;
}

namespace {
void write_controls(std::ostream& destination, const ControlSettings& c, const std::string& prefix) {
    std::ostringstream out;
    auto binding = [&](std::string_view key, const Binding& b) {
        out << key << " = " << b.text() << '\n';
        if (!b.name.empty()) out << key << "_name = " << b.name << '\n';
    };
    for (std::size_t i = 0; i < c.buttons.size(); ++i) binding(button_keys[i], c.buttons[i]);
    out << "steering = " << (c.steering == Steering::wheel ? "wheel" : c.steering == Steering::mouse ? "mouse" : "keys") << '\n';
    binding("wheel_axis", c.wheel_axis);
    out << strf("wheel_rotation = %.9g\nwheel_counts_per_turn = %.9g\nwheel_invert = %d\n",
                c.wheel.rotation, c.wheel.counts_per_turn, c.wheel.invert ? 1 : 0);
    out << "moza_angle = " << (c.moza_angle ? 1 : 0) << "\nmoza_sdk = " << c.moza_sdk << '\n';
    binding("pedal_axis", c.pedal_axis);
    out << strf("pedal_released = %.9g\npedal_floored = %.9g\npedal_deadzone = %.9g\n", c.pedal.released, c.pedal.floored, c.pedal.deadzone);
    out << strf("mouse_counts = %.9g\nstick_speed = %.9g\nstick_curve = %.9g\nfriction = %d\n", c.mouse_counts, c.stick_speed, c.stick_curve, c.friction);
    std::istringstream lines(out.str());
    for (std::string line; std::getline(lines, line);) destination << prefix << line << '\n';
}
}
void Settings::remember_controls() {
    if (steering == Steering::wheel && wheel_axis.device.size() == 32 && wheel_axis.device.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
        profiles[wheel_axis.device] = static_cast<const ControlSettings&>(*this);
}
bool Settings::select_profile(const std::string& guid) {
    const auto it = profiles.find(guid);
    if (it == profiles.end()) return false;
    static_cast<ControlSettings&>(*this) = it->second;
    return true;
}
bool Settings::save(const std::string& path) const {
    std::ostringstream out;
    out << "# apb settings, written by the game's settings menu (F1).\n";
    write_controls(out, *this, "");
    const char* scaling = display.scaling == gfx::Scaling::fit ? "fit" : display.scaling == gfx::Scaling::stretch ? "stretch" : "integer";
    out << strf("scaling = %s\nsmooth = %d\nvsync = %d\nmatch_display = %d\nfullscreen = %d\n", scaling, display.smooth ? 1 : 0,
                display.vsync ? 1 : 0, display.match_display ? 1 : 0, display.fullscreen ? 1 : 0);
    const char* effect = display.effect == gfx::Effect::pixel_art ? "pixel_art" : display.effect == gfx::Effect::crt ? "crt" : "none";
    out << strf("effect = %s\ncrt_scanlines = %.9g\ncrt_mask = %.9g\n", effect, display.scanlines, display.mask);
    out << "monitor = " << display.monitor << "\ngpu = " << display.gpu << '\n';
    out << "performance_overlay = " << (display.performance_overlay ? 1 : 0) << '\n';
    out << "volume = " << sound.volume << '\n';
    out << "rom_dir = " << rom_dir << '\n';
    auto saved_profiles = profiles;
    if (steering == Steering::wheel && wheel_axis.device.size() == 32 && wheel_axis.device.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
        saved_profiles[wheel_axis.device] = static_cast<const ControlSettings&>(*this);
    for (const auto& [guid, profile] : saved_profiles) write_controls(out, profile, "profile." + guid + ".");
    return out && persist::replace(path, out.str());
}

// ---- Devices ----------------------------------------------------------------

Input::Input() {
    // SDL_INIT_GAMEPAD does not initialise force feedback. A wheel can
    // steer without haptics, so a haptic driver failure is non-fatal.
    haptics_ready_ = SDL_InitSubSystem(SDL_INIT_HAPTIC);
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
        for (int i = 0; i < count; ++i) add(ids[i]);
        SDL_free(ids);
    }
}

Input::~Input() {
    for (auto& d : devices_) {
        if (d.haptic) {
            SDL_StopHapticEffects(d.haptic);
            SDL_CloseHaptic(d.haptic);
        }
        if (d.gamepad) SDL_CloseGamepad(d.gamepad);
        if (d.joystick) SDL_CloseJoystick(d.joystick);
    }
    if (haptics_ready_) SDL_QuitSubSystem(SDL_INIT_HAPTIC);
}

const char* Input::type_name(SDL_JoystickType type) {
    switch (type) {
    case SDL_JOYSTICK_TYPE_GAMEPAD: return "gamepad";
    case SDL_JOYSTICK_TYPE_WHEEL: return "wheel";
    case SDL_JOYSTICK_TYPE_ARCADE_STICK: return "arcade stick";
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK: return "flight stick";
    case SDL_JOYSTICK_TYPE_DANCE_PAD: return "dance pad";
    case SDL_JOYSTICK_TYPE_GUITAR: return "guitar";
    case SDL_JOYSTICK_TYPE_DRUM_KIT: return "drum kit";
    case SDL_JOYSTICK_TYPE_ARCADE_PAD: return "arcade pad";
    case SDL_JOYSTICK_TYPE_THROTTLE: return "throttle or pedals";
    default: return "controller";
    }
}

Input::Device* Input::find(const std::string& guid) {
    for (auto& d : devices_)
        if (d.guid == guid) return &d;
    return nullptr;
}
const Input::Device* Input::find(const std::string& guid) const {
    for (const auto& d : devices_)
        if (d.guid == guid) return &d;
    return nullptr;
}

void Input::add(SDL_JoystickID id) {
    if (std::ranges::any_of(devices_, [&](const Device& d) { return d.id == id; })) return;
    Device d;
    d.id = id;
    d.joystick = SDL_OpenJoystick(id);
    if (!d.joystick) {
        const char* name = SDL_GetJoystickNameForID(id);
        std::fprintf(stderr, "apb: couldn't open the controller %s: %s\n", name ? name : "(no name)", SDL_GetError());
        return;
    }
    if (SDL_IsGamepad(id)) d.gamepad = SDL_OpenGamepad(id);
    d.guid = guid_of(id);
    const char* name = SDL_GetJoystickName(d.joystick);
    d.name = name ? name : "controller";
    d.type = SDL_GetJoystickType(d.joystick);
    const int axes = SDL_GetNumJoystickAxes(d.joystick);
    for (int axis = 0; axis < axes; ++axis) {
        const int value = SDL_GetJoystickAxis(d.joystick, axis);
        d.axis_min.push_back(value); d.axis_max.push_back(value);
    }
    std::printf("apb: controller connected: %s (%s%s)\n", d.name.c_str(), type_name(d.type),
                d.gamepad ? "; buttons and sticks as a gamepad" : "; raw axes and buttons only");
    std::fflush(stdout);
    devices_.push_back(std::move(d));
    if (devices_.back().guid == settings.wheel_axis.device) reset_motion();
    if (!active_profile_.empty() && !find(settings.wheel_axis.device) && settings.profiles.contains(devices_.back().guid)) configure();
    else start_force(devices_.back());
}

void Input::remove(SDL_JoystickID id) {
    const auto it = std::ranges::find_if(devices_, [&](const Device& d) { return d.id == id; });
    if (it == devices_.end()) return;
    if (it->guid == settings.wheel_axis.device) reset_motion();
    if (it->haptic) {
        SDL_StopHapticEffects(it->haptic);
        SDL_CloseHaptic(it->haptic);
    }
    if (it->gamepad) SDL_CloseGamepad(it->gamepad);
    SDL_CloseJoystick(it->joystick);
    std::printf("apb: controller disconnected: %s\n", it->name.c_str());
    std::fflush(stdout);
    devices_.erase(it);
    if (!find(settings.wheel_axis.device)) configure();
}

// The wheel the steering is bound to gets the cabinet's feel: no centring
// spring (APB's wheel spins freely) and, if set, some friction (or damping,
// where the wheel has no friction effect), so it doesn't spin on its own.
void Input::start_force(Device& d) {
    const bool bound = playing && settings.steering == Steering::wheel && settings.wheel_axis.device == d.guid;
    if (!bound) {
        if (d.haptic) {
            SDL_StopHapticEffects(d.haptic);
            SDL_CloseHaptic(d.haptic);
            d.haptic = nullptr;
            d.effect = -1;
        }
        d.force_status.clear();
        return;
    }
    if (!haptics_ready_) { d.force_status = "Force feedback unavailable: SDL haptics could not start"; return; }
    if (!d.haptic) d.haptic = SDL_OpenHapticFromJoystick(d.joystick);
    if (!d.haptic) { d.force_status = std::string("Force feedback unavailable: ") + SDL_GetError(); return; }
    const Uint32 features = SDL_GetHapticFeatures(d.haptic);
    const bool no_centre = !(features & SDL_HAPTIC_AUTOCENTER) || SDL_SetHapticAutocenter(d.haptic, 0);
    if (features & SDL_HAPTIC_GAIN) SDL_SetHapticGain(d.haptic, 100);
    const Uint16 type = (features & SDL_HAPTIC_FRICTION) ? SDL_HAPTIC_FRICTION : (features & SDL_HAPTIC_DAMPER) ? SDL_HAPTIC_DAMPER : 0;
    if (d.effect >= 0) {
        SDL_DestroyHapticEffect(d.haptic, d.effect);
        d.effect = -1;
    }
    if (!type || settings.friction <= 0) {
        d.force_status = settings.friction <= 0 ? "Game friction is off" : "This driver offers neither friction nor damping";
        if (!no_centre) d.force_status += "; could not turn off driver centring";
        return;
    }
    SDL_HapticEffect effect{};
    effect.condition.type = type;
    effect.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.condition.direction.dir[0] = 1;
    effect.condition.length = SDL_HAPTIC_INFINITY;
    const auto level = static_cast<Uint16>(0xffff * settings.friction / 100);
    const auto coeff = static_cast<Sint16>(0x7fff * settings.friction / 100);
    for (int i = 0; i < 3; ++i) {
        effect.condition.right_sat[i] = effect.condition.left_sat[i] = level;
        effect.condition.right_coeff[i] = effect.condition.left_coeff[i] = coeff;
    }
    d.effect = SDL_CreateHapticEffect(d.haptic, &effect);
    if (d.effect >= 0 && SDL_RunHapticEffect(d.haptic, d.effect, 1)) {
        d.force_status = type == SDL_HAPTIC_FRICTION ? "Friction active" : "Damping active (driver has no friction effect)";
    } else {
        d.force_status = std::string("Force effect could not start: ") + SDL_GetError();
    }
    if (!no_centre) d.force_status += "; could not turn off driver centring";
}

void Input::configure() {
    // The wheel in use until now keeps every change made to its setup
    // (buttons, pedal, mouse) before another one's is restored.
    if (active_profile_ == settings.wheel_axis.device) settings.remember_controls();
    // Known wheel selected in the menu, or a different known wheel connected
    // while the selected one is absent: restore that wheel's whole setup.
    std::string selected = settings.wheel_axis.device;
    if (settings.steering == Steering::wheel && !find(selected))
        for (const auto& d : devices_) if (settings.profiles.contains(d.guid)) { selected = d.guid; break; }
    if (settings.steering == Steering::wheel && selected != active_profile_ && settings.profiles.contains(selected)) settings.select_profile(selected);
    active_profile_ = settings.wheel_axis.device;
    settings.remember_controls();
    wheel_.configure(settings.wheel);
    stick_.counts_at_full = settings.stick_speed;
    stick_.curve = settings.stick_curve;
    const bool want_moza = settings.steering == Steering::wheel && settings.moza_angle;
    const char* base = SDL_GetBasePath();
    const std::string path = settings.moza_sdk.empty() ? std::string(base ? base : "") + "MOZA_SDK.dll" : settings.moza_sdk;
    if (want_moza && (path != moza_path_ || !moza_.is_open())) {
        moza_.open(path);
        moza_path_ = path;
    } else if (!want_moza) {
        moza_.close();
        moza_path_.clear();
    }
    reset_motion();
    for (auto& d : devices_) start_force(d);
}

void Input::reset_motion() {
    wheel_.reset();
    stick_.reset();
    mouse_dx_ = mouse_carry_ = 0;
    pedal_ = 0;
    moza_degrees_.reset();
    if (settings.steering == Steering::wheel && !settings.moza_angle)
        if (const Device* d = find(settings.wheel_axis.device)) wheel_.observe(axis_of(d->joystick, settings.wheel_axis.index));
}

void Input::set_playing(bool value) {
    requested_playing_ = value;
    const bool effective = value && focused_;
    if (effective == was_playing_) return;
    resuming_ = effective && !was_playing_;
    playing = effective;
    was_playing_ = effective;
    reset_motion();
    for (auto& d : devices_) start_force(d);
}
void Input::set_focused(bool value) {
    focused_ = value;
    set_playing(requested_playing_);
}

void Input::begin_wheel_measurement() {
    measurement_.reset();
    measurement_.observe(read(settings.wheel_axis));
    measuring_wheel_ = true;
}

// ---- Reading ----------------------------------------------------------------

double Input::read(const Binding& b) const {
    switch (b.kind) {
    case Binding::Kind::key: {
        int count = 0;
        const bool* keys = SDL_GetKeyboardState(&count);
        return b.index >= 0 && b.index < count && keys[b.index] ? 1 : 0;
    }
    case Binding::Kind::button:
        if (const Device* d = find(b.device)) return SDL_GetJoystickButton(d->joystick, b.index) ? 1 : 0;
        return 0;
    case Binding::Kind::axis:
        if (const Device* d = find(b.device)) {
            const double v = axis_of(d->joystick, b.index);
            return b.value == 0 ? v : std::max(0.0, v * b.value);
        }
        return 0;
    case Binding::Kind::hat:
        if (const Device* d = find(b.device)) return (SDL_GetJoystickHat(d->joystick, b.index) & b.value) ? 1 : 0;
        return 0;
    default: return 0;
    }
}

void Input::handle(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MINIMIZED: set_focused(false); break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: set_focused(true); break;
    case SDL_EVENT_JOYSTICK_ADDED: add(e.jdevice.which); break;
    case SDL_EVENT_JOYSTICK_REMOVED: remove(e.jdevice.which); break;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        for (auto& d : devices_) if (d.id == e.jaxis.which && e.jaxis.axis < d.axis_min.size()) {
            d.axis_min[e.jaxis.axis] = std::min(d.axis_min[e.jaxis.axis], static_cast<int>(e.jaxis.value));
            d.axis_max[e.jaxis.axis] = std::max(d.axis_max[e.jaxis.axis], static_cast<int>(e.jaxis.value));
        }
        if (settings.steering == Steering::wheel && e.jaxis.axis == settings.wheel_axis.index) {
            const Device* d = find(settings.wheel_axis.device);
            if (d && d->id == e.jaxis.which) {
                const double axis = e.jaxis.value < 0 ? e.jaxis.value / 32768.0 : e.jaxis.value / 32767.0;
                if (playing && !resuming_ && !settings.moza_angle) wheel_.observe(axis);
                if (measuring_wheel_) measurement_.observe(axis);
            }
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (playing) mouse_dx_ += e.motion.xrel;
        break;
    case SDL_EVENT_KEY_DOWN:
        // Escape, F1, F2, F3 and F11 are the program's (the menu, the test
        // switch, frame times, full screen), never a cabinet button.
        if (listen_ == Listen::button && !e.key.repeat && e.key.scancode != SDL_SCANCODE_ESCAPE && e.key.scancode != SDL_SCANCODE_F1 &&
            e.key.scancode != SDL_SCANCODE_F2 && e.key.scancode != SDL_SCANCODE_F3 && e.key.scancode != SDL_SCANCODE_F11) {
            heard_ = Binding{Binding::Kind::key, {}, {}, static_cast<int>(e.key.scancode), 0};
            listen_ = Listen::none;
        }
        break;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        if (listen_ == Listen::button) {
            const std::string guid = guid_of(e.jbutton.which);
            const Device* d = find(guid);
            heard_ = Binding{Binding::Kind::button, guid, d ? d->name : "", e.jbutton.button, 0};
            listen_ = Listen::none;
        }
        break;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        if (listen_ == Listen::button && (e.jhat.value == SDL_HAT_UP || e.jhat.value == SDL_HAT_DOWN || e.jhat.value == SDL_HAT_LEFT ||
                                          e.jhat.value == SDL_HAT_RIGHT)) {
            const std::string guid = guid_of(e.jhat.which);
            const Device* d = find(guid);
            heard_ = Binding{Binding::Kind::hat, guid, d ? d->name : "", e.jhat.hat, e.jhat.value};
            listen_ = Listen::none;
        }
        break;
    default: break;
    }
}

// Every axis as it is now, so listening hears what moves from here.
void Input::baseline() {
    rest_.clear();
    for (const auto& d : devices_) {
        Rest r{d.guid, d.name, {}};
        for (int a = 0; a < SDL_GetNumJoystickAxes(d.joystick); ++a) r.axes.push_back(axis_of(d.joystick, a));
        rest_.push_back(std::move(r));
    }
}

void Input::listen(Listen what) {
    listen_ = what;
    heard_.reset();
    pedal_candidate_ = {};
    baseline();
}

std::optional<Binding> Input::heard() {
    auto h = heard_;
    heard_.reset();
    return h;
}

void Input::apply(sys2::Inputs& in) {
    profile::Scope measured(playing ? profiler : nullptr, profile::Stage::input);
    if (playing != was_playing_) set_playing(playing);
    if (resuming_) {
        reset_motion();
        resuming_ = false;
        start_from_menu_ = true;   // cleared below unless a Start is still down
        held_from_menu_ = true;    // the buttons too (A or Space on Close, say): each ignored until let go
    }
    if (measuring_wheel_) measurement_.observe(read(settings.wheel_axis));
    // Listening: the axis that has moved furthest from where it was.
    if (listen_ == Listen::wheel || listen_ == Listen::pedal || listen_ == Listen::button) {
        double best = 0;
        Binding found;
        double rest_value = 0, now_value = 0;
        for (const auto& r : rest_) {
            const Device* d = find(r.guid);
            if (!d) continue;
            for (std::size_t a = 0; a < r.axes.size(); ++a) {
                const double now = axis_of(d->joystick, static_cast<int>(a));
                const double moved = std::abs(now - r.axes[a]);
                if (moved > best) {
                    best = moved;
                    found = Binding{Binding::Kind::axis, r.guid, r.name, static_cast<int>(a), 0};
                    rest_value = r.axes[a];
                    now_value = now;
                }
            }
        }
        if (listen_ == Listen::wheel && best > 0.12) {
            heard_ = found;
            listen_ = Listen::none;
        } else if (listen_ == Listen::button && best > 0.6) {
            // An axis pressed like a button (a trigger, a pedal): that way round.
            found.value = now_value > rest_value ? 1 : -1;
            heard_ = found;
            listen_ = Listen::none;
        } else if (listen_ == Listen::pedal && best > 0.3) {
            // The pedal: keep the furthest reading seen; the menu's Done ends it.
            if (pedal_candidate_.empty() || pedal_candidate_.device != found.device || pedal_candidate_.index != found.index) {
                pedal_candidate_ = found;
                pedal_rest_ = rest_value;
                pedal_far_ = now_value;
            } else if (std::abs(now_value - pedal_rest_) > std::abs(pedal_far_ - pedal_rest_)) {
                pedal_far_ = now_value;
            }
        }
    }

    const bool* key = SDL_GetKeyboardState(nullptr);
    // The gamepad defaults. Buttons (A/B/X, Start, Back, the d-pad) come
    // from every device SDL knows as a gamepad, a wheel's own buttons too.
    // The stick and the trigger don't come from a device bound as the wheel
    // or the pedal: SDL knows some wheels as gamepads (their wheel as the
    // left stick, their pedals as triggers), and that would steer twice, or
    // hold the gas down where the mapping has a pedal the wrong way round.
    // (A real gamepad with one trigger bound as the pedal keeps its stick.)
    const bool wheel_bound = settings.steering == Steering::wheel && !settings.wheel_axis.empty();
    auto is_wheel = [&](const Device& d) { return wheel_bound && settings.wheel_axis.device == d.guid; };
    auto is_pedal = [&](const Device& d) { return !settings.pedal_axis.empty() && settings.pedal_axis.device == d.guid; };
    auto stick_from = [&](const Device& d) {
        return d.gamepad && !is_wheel(d) && (!is_pedal(d) || d.type == SDL_JOYSTICK_TYPE_GAMEPAD);
    };
    auto trigger_from = [&](const Device& d) { return d.gamepad && !is_wheel(d) && !is_pedal(d); };
    auto pad = [&](SDL_GamepadButton b) {
        return std::ranges::any_of(devices_, [&](const Device& d) { return d.gamepad && SDL_GetGamepadButton(d.gamepad, b); });
    };
    auto bound = [&](Button b) { return playing && settings.buttons[static_cast<int>(b)].kind != Binding::Kind::none &&
                                        read(settings.buttons[static_cast<int>(b)]) > 0.5; };
    const bool live = playing;   // while the menu is open, the cabinet's controls rest
    const bool reading = live || focused_;   // the menu's meters: read while it's open, not while another window has the controls

    // Back (coin 1) and Start (siren/start), which together are the menu's.
    const bool back = pad(SDL_GAMEPAD_BUTTON_BACK), start = pad(SDL_GAMEPAD_BUTTON_START);
    if (back && start) menu_combo_ = true;
    else if (!back && !start) menu_combo_ = false;
    if (!start) start_from_menu_ = false;
    back_frames_ = back ? std::min(back_frames_ + 1, 1000) : 0;
    start_frames_ = start ? std::min(start_frames_ + 1, 1000) : 0;
    // 3 frames: 50 ms, shorter than any press, longer than two thumbs landing apart.
    const bool back_coin = back_frames_ >= 3 && !menu_combo_;
    const bool start_press = start_frames_ >= 3 && !menu_combo_ && !start_from_menu_;

    // IN0, active low: siren/start, fire and the self-test switch.
    unsigned in0 = 0xffff;
    const bool b1_raw = key[SDL_SCANCODE_LCTRL] || key[SDL_SCANCODE_Z] || pad(SDL_GAMEPAD_BUTTON_SOUTH) || bound(Button::one);
    const bool b2 = key[SDL_SCANCODE_LALT] || key[SDL_SCANCODE_X] || pad(SDL_GAMEPAD_BUTTON_EAST) || bound(Button::two);
    const bool b3 = key[SDL_SCANCODE_SPACE] || key[SDL_SCANCODE_C] || pad(SDL_GAMEPAD_BUTTON_WEST) || bound(Button::three);
    const bool c1 = key[SDL_SCANCODE_5] || bound(Button::coin_left);
    const bool c2 = key[SDL_SCANCODE_6] || bound(Button::coin_right);
    const bool c3 = key[SDL_SCANCODE_9] || bound(Button::service);
    // Held when play resumed: ignored until released (bit per control).
    const unsigned held = (b1_raw ? 1u : 0u) | (b2 ? 2u : 0u) | (b3 ? 4u : 0u) | (c1 ? 8u : 0u) | (c2 ? 16u : 0u) | (c3 ? 32u : 0u);
    if (held_from_menu_) {
        held_mask_ = held;
        held_from_menu_ = false;
    }
    held_mask_ &= held;
    const unsigned down = held & ~held_mask_;
    const bool b1 = (down & 1u) || start_press;
    if (live && b1) in0 &= ~unsigned(in0_bits[0]);
    if (live && (down & 2u)) in0 &= ~unsigned(in0_bits[1]);
    if (live && (down & 4u)) in0 &= ~unsigned(in0_bits[2]);
    if (test_switch) in0 &= ~0x8000u;
    in.in0 = static_cast<std::uint16_t>(in0);
    // IN1, active low: the coins and the service coin (the low bits are the board's own).
    unsigned in1 = 0xf0;
    if (live && ((down & 8u) || back_coin)) in1 &= ~unsigned(in1_bits[0]);
    if (live && (down & 16u)) in1 &= ~unsigned(in1_bits[1]);
    if (live && (down & 32u)) in1 &= ~unsigned(in1_bits[2]);
    in.in1 = static_cast<std::uint8_t>(in1);

    // The gas pedal, ADC1. A bound pedal axis gives its travel; otherwise the
    // Up key, a d-pad's up and gamepad triggers move a pedal 8 steps a frame,
    // as a foot would.
    double travel = 0;
    if (!settings.pedal_axis.empty() && find(settings.pedal_axis.device)) {
        const Device* d = find(settings.pedal_axis.device);
        travel = controls::pedal_travel(axis_of(d->joystick, settings.pedal_axis.index), settings.pedal);
    }
    int target = key[SDL_SCANCODE_UP] || pad(SDL_GAMEPAD_BUTTON_DPAD_UP) ? 0x3f : 0;
    for (const auto& d : devices_)
        if (trigger_from(d)) target = std::max(target, SDL_GetGamepadAxis(d.gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 0x3f / 32767);
    pedal_ = target > pedal_ ? std::min(target, pedal_ + 8) : std::max(target, pedal_ - 8);
    travel = std::max(travel, pedal_ / 63.0);
    last_pedal = reading ? travel : 0;   // the menu's meter shows it while the game rests
    if (!live) travel = 0;
    in.adc[1] = controls::pedal_adc(travel);

    // The steering wheel: a free-spinning dial on LETA0, counts a frame.
    // The arrow keys and a d-pad ask for 60% of the full speed; else the stick pushed furthest.
    const double keys = (key[SDL_SCANCODE_RIGHT] || pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? 0.6 : 0) -
                        (key[SDL_SCANCODE_LEFT] || pad(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? 0.6 : 0);
    double stick = 0;
    for (const auto& d : devices_)
        if (stick_from(d)) {
            const double x = SDL_GetGamepadAxis(d.gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0;
            if (std::abs(x) > std::abs(stick)) stick = x;
        }
    int counts = keys != 0 ? stick_.update_linear(keys) : stick_.update(stick);
    if (settings.steering == Steering::wheel) {
        if (settings.moza_angle) {
            moza_degrees_ = moza_.read();
            if (moza_degrees_) {
                moza_min_ = moza_min_ ? std::min(*moza_min_, *moza_degrees_) : *moza_degrees_;
                moza_max_ = moza_max_ ? std::max(*moza_max_, *moza_degrees_) : *moza_degrees_;
            }
            if (moza_degrees_ && reading) counts += wheel_.update_degrees(*moza_degrees_);   // in the menu too, for its reading
            else wheel_.reset();
        } else if (!settings.wheel_axis.empty()) {
            if (const Device* d = find(settings.wheel_axis.device); d && reading)   // in the menu too, for its reading (resuming rebases it)
                counts += wheel_.update(axis_of(d->joystick, settings.wheel_axis.index));
            else wheel_.reset();   // unplugged or away: a fresh baseline when it's back
        }
    }
    if (settings.steering == Steering::mouse) {
        mouse_carry_ += mouse_dx_ * settings.mouse_counts;
        const double whole = std::trunc(mouse_carry_);
        mouse_carry_ -= whole;
        counts += static_cast<int>(whole);
    }
    mouse_dx_ = 0;
    last_counts = reading ? counts : 0;   // the menu's "Dial counts this frame"
    if (!live) {   // the cabinet's controls rest; resuming rebases the wheel (reset_motion)
        counts = 0;
        stick_.reset();
        mouse_carry_ = 0;
        pedal_ = 0;
    }
    in.leta[0] = static_cast<std::uint8_t>(in.leta[0] + counts);
}

std::string Input::diagnostics() const {
    std::ostringstream out;
    out << "APB controller diagnostics " << QILIN_APP_VERSION << "\nPlatform: " << SDL_GetPlatform()
        << "\nSDL version: " << SDL_GetVersion() << "\nFocused: " << focused_ << "  Playing: " << playing
        << "\nSteering source: " << (settings.moza_angle ? "MOZA physical angle" : settings.steering == Steering::wheel ? "SDL wheel axis" : settings.steering == Steering::mouse ? "mouse/spinner" : "keys/gamepad")
        << "\nWheel binding: " << settings.wheel_axis.describe()
        << "\nDegrees per wrapping axis cycle: " << settings.wheel.rotation
        << "\nEncoder counts per turn: " << settings.wheel.counts_per_turn
        << "\nInvert: " << settings.wheel.invert << "  Friction percent: " << settings.friction
        << "\nPedal binding: " << settings.pedal_axis.describe()
        << "\nPedal calibration released/floored/deadzone: " << settings.pedal.released << " / " << settings.pedal.floored << " / " << settings.pedal.deadzone
        << "\nMOZA status: " << moza_.status() << "\n";
    if (moza_degrees_) out << "Last physical angle: " << *moza_degrees_ << " degrees\n";
    if (moza_min_ && moza_max_) out << "Observed SDK angle min/max: " << *moza_min_ << " / " << *moza_max_ << " degrees\n";
    out << "Supported MOZA SDK ABI: 1.0.1.8 Windows x64\n";
    out << "Turn each wheel through its intended range and fully press/release pedals before exporting.\n"
        << "A wheel must turn freely: an SDL axis that stops at an endpoint is a steering lock or a clipped axis, which can't drive APB's wheel.\n";
    for (const auto& d : devices_) {
        const char* serial = SDL_GetJoystickSerial(d.joystick);
        const char* path = SDL_GetJoystickPath(d.joystick);
        out << "\nDevice: " << d.name << "\nGUID: " << d.guid << "\nType: " << type_name(d.type)
            << "\nVendor/product: " << SDL_GetJoystickVendor(d.joystick) << " / " << SDL_GetJoystickProduct(d.joystick)
            << "\nSerial: " << (serial ? serial : "unavailable") << "\nPath: " << (path ? path : "unavailable")
            << "\nButtons/hats: " << SDL_GetNumJoystickButtons(d.joystick) << " / " << SDL_GetNumJoystickHats(d.joystick)
            << "\nForce status: " << (d.force_status.empty() ? "not active on this device" : d.force_status) << "\n";
        for (std::size_t axis = 0; axis < d.axis_min.size(); ++axis)
            out << "Axis " << axis << ": current=" << SDL_GetJoystickAxis(d.joystick, static_cast<int>(axis))
                << " observed_min=" << d.axis_min[axis] << " observed_max=" << d.axis_max[axis]
                << " reached_endpoint=" << (d.axis_min[axis] <= -32760 || d.axis_max[axis] >= 32760) << "\n";
    }
    return out.str();
}

}  // namespace qilin::app
