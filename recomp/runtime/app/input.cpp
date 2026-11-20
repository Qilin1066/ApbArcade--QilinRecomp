#include "input.hpp"

#include <cstdio>
#include "text.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <map>

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
    std::ifstream in(path);
    if (!in) return false;
    std::map<std::string, std::string, std::less<>> kv;
    for (std::string line; std::getline(in, line);) {
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
    auto get = [&](std::string_view key) -> std::string_view {
        const auto it = kv.find(key);
        return it == kv.end() ? std::string_view() : std::string_view(it->second);
    };
    auto num = [&](std::string_view key, double fallback) { return get(key).empty() ? fallback : number(get(key), fallback); };
    auto binding = [&](std::string_view key) {
        Binding b = Binding::parse(get(key));
        b.name = std::string(get(std::string(key) + "_name"));
        return b;
    };
    for (std::size_t i = 0; i < buttons.size(); ++i) buttons[i] = binding(button_keys[i]);
    const auto steer = get("steering");
    steering = steer == "wheel" ? Steering::wheel : steer == "mouse" ? Steering::mouse : Steering::keys_and_pad;
    wheel_axis = binding("wheel_axis");
    wheel.rotation = std::clamp(num("wheel_rotation", wheel.rotation), 90.0, 10000.0);
    wheel.counts_per_turn = std::clamp(num("wheel_counts_per_turn", wheel.counts_per_turn), 8.0, 2048.0);
    wheel.edge = std::clamp(num("wheel_edge", wheel.edge), 0.5, 1.0);
    wheel.edge_rate = std::clamp(num("wheel_edge_rate", wheel.edge_rate), 0.0, 32.0);
    wheel.invert = num("wheel_invert", 0) != 0;
    pedal_axis = binding("pedal_axis");
    pedal.released = num("pedal_released", pedal.released);
    pedal.floored = num("pedal_floored", pedal.floored);
    pedal.deadzone = std::clamp(num("pedal_deadzone", pedal.deadzone), 0.0, 0.3);
    mouse_counts = std::clamp(num("mouse_counts", mouse_counts), 0.01, 8.0);
    stick_speed = std::clamp(num("stick_speed", stick_speed), 0.5, 16.0);
    stick_curve = std::clamp(num("stick_curve", stick_curve), 1.0, 4.0);
    friction = static_cast<int>(std::clamp(num("friction", friction), 0.0, 100.0));
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
    rom_dir = std::string(get("rom_dir"));
    return true;
}

bool Settings::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "# apb settings, written by the game's settings menu (F1).\n";
    auto binding = [&](std::string_view key, const Binding& b) {
        out << key << " = " << b.text() << '\n';
        if (!b.name.empty()) out << key << "_name = " << b.name << '\n';
    };
    for (std::size_t i = 0; i < buttons.size(); ++i) binding(button_keys[i], buttons[i]);
    out << "steering = " << (steering == Steering::wheel ? "wheel" : steering == Steering::mouse ? "mouse" : "keys") << '\n';
    binding("wheel_axis", wheel_axis);
    out << strf("wheel_rotation = %.9g\nwheel_counts_per_turn = %.9g\nwheel_edge = %.9g\nwheel_edge_rate = %.9g\nwheel_invert = %d\n",
                wheel.rotation, wheel.counts_per_turn, wheel.edge, wheel.edge_rate, wheel.invert ? 1 : 0);
    binding("pedal_axis", pedal_axis);
    out << strf("pedal_released = %.9g\npedal_floored = %.9g\npedal_deadzone = %.9g\n", pedal.released, pedal.floored, pedal.deadzone);
    out << strf("mouse_counts = %.9g\nstick_speed = %.9g\nstick_curve = %.9g\nfriction = %d\n", mouse_counts, stick_speed, stick_curve, friction);
    const char* scaling = display.scaling == gfx::Scaling::fit ? "fit" : display.scaling == gfx::Scaling::stretch ? "stretch" : "integer";
    out << strf("scaling = %s\nsmooth = %d\nvsync = %d\nmatch_display = %d\nfullscreen = %d\n", scaling, display.smooth ? 1 : 0,
                display.vsync ? 1 : 0, display.match_display ? 1 : 0, display.fullscreen ? 1 : 0);
    const char* effect = display.effect == gfx::Effect::pixel_art ? "pixel_art" : display.effect == gfx::Effect::crt ? "crt" : "none";
    out << strf("effect = %s\ncrt_scanlines = %.9g\ncrt_mask = %.9g\n", effect, display.scanlines, display.mask);
    out << "monitor = " << display.monitor << "\ngpu = " << display.gpu << '\n';
    out << "rom_dir = " << rom_dir << '\n';
    return static_cast<bool>(out);
}

// ---- Devices ----------------------------------------------------------------

Input::Input() {
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
    std::printf("apb: controller connected: %s (%s%s)\n", d.name.c_str(), type_name(d.type),
                d.gamepad ? "; buttons and sticks as a gamepad" : "; raw axes and buttons only");
    std::fflush(stdout);
    devices_.push_back(std::move(d));
    start_force(devices_.back());
}

void Input::remove(SDL_JoystickID id) {
    const auto it = std::ranges::find_if(devices_, [&](const Device& d) { return d.id == id; });
    if (it == devices_.end()) return;
    if (it->haptic) SDL_CloseHaptic(it->haptic);
    if (it->gamepad) SDL_CloseGamepad(it->gamepad);
    SDL_CloseJoystick(it->joystick);
    std::printf("apb: controller disconnected: %s\n", it->name.c_str());
    std::fflush(stdout);
    devices_.erase(it);
}

// The wheel the steering is bound to gets the cabinet's feel: no centring
// spring (APB's wheel spins freely) and, if set, some friction (or damping,
// where the wheel has no friction effect), so it doesn't spin on its own.
void Input::start_force(Device& d) {
    const bool bound = settings.steering == Steering::wheel && settings.wheel_axis.device == d.guid;
    if (!bound) {
        if (d.haptic) {
            SDL_StopHapticEffects(d.haptic);
            SDL_CloseHaptic(d.haptic);
            d.haptic = nullptr;
            d.effect = -1;
        }
        return;
    }
    if (!d.haptic) d.haptic = SDL_OpenHapticFromJoystick(d.joystick);
    if (!d.haptic) return;
    const Uint32 features = SDL_GetHapticFeatures(d.haptic);
    if (features & SDL_HAPTIC_AUTOCENTER) SDL_SetHapticAutocenter(d.haptic, 0);
    if (features & SDL_HAPTIC_GAIN) SDL_SetHapticGain(d.haptic, 100);
    const Uint16 type = (features & SDL_HAPTIC_FRICTION) ? SDL_HAPTIC_FRICTION : (features & SDL_HAPTIC_DAMPER) ? SDL_HAPTIC_DAMPER : 0;
    if (d.effect >= 0) {
        SDL_DestroyHapticEffect(d.haptic, d.effect);
        d.effect = -1;
    }
    if (!type || settings.friction <= 0) return;
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
    if (d.effect >= 0) SDL_RunHapticEffect(d.haptic, d.effect, 1);
}

void Input::configure() {
    controls::WheelSettings w = settings.wheel;
    wheel_.configure(w);
    wheel_.reset();
    stick_.counts_at_full = settings.stick_speed;
    stick_.curve = settings.stick_curve;
    for (auto& d : devices_) start_force(d);
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
    case SDL_EVENT_JOYSTICK_ADDED: add(e.jdevice.which); break;
    case SDL_EVENT_JOYSTICK_REMOVED: remove(e.jdevice.which); break;
    case SDL_EVENT_MOUSE_MOTION:
        if (playing) mouse_dx_ += e.motion.xrel;
        break;
    case SDL_EVENT_KEY_DOWN:
        // Escape, F1 and F2 are the menu's and the test switch's, never a cabinet button.
        if (listen_ == Listen::button && !e.key.repeat && e.key.scancode != SDL_SCANCODE_ESCAPE && e.key.scancode != SDL_SCANCODE_F1 &&
            e.key.scancode != SDL_SCANCODE_F2) {
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
    // The gamepad defaults (stick, trigger, A/B/X, Back) come from gamepads
    // that aren't bound as the wheel or the pedal: SDL knows some wheels as
    // gamepads (their wheel as the left stick, their pedals as triggers), and
    // that would steer twice.
    auto defaults_from = [&](const Device& d) {
        if (!d.gamepad) return false;
        if (settings.steering == Steering::wheel && settings.wheel_axis.device == d.guid) return false;
        return settings.pedal_axis.device != d.guid;
    };
    auto pad = [&](SDL_GamepadButton b) {
        return std::ranges::any_of(devices_, [&](const Device& d) { return defaults_from(d) && SDL_GetGamepadButton(d.gamepad, b); });
    };
    auto bound = [&](Button b) { return playing && settings.buttons[static_cast<int>(b)].kind != Binding::Kind::none &&
                                        read(settings.buttons[static_cast<int>(b)]) > 0.5; };
    const bool live = playing;   // while the menu is open, the cabinet's controls rest

    // IN0, active low: siren/start, fire and the self-test switch.
    unsigned in0 = 0xffff;
    const bool b1 = key[SDL_SCANCODE_LCTRL] || key[SDL_SCANCODE_Z] || pad(SDL_GAMEPAD_BUTTON_SOUTH) || bound(Button::one);
    const bool b2 = key[SDL_SCANCODE_LALT] || key[SDL_SCANCODE_X] || pad(SDL_GAMEPAD_BUTTON_EAST) || bound(Button::two);
    const bool b3 = key[SDL_SCANCODE_SPACE] || key[SDL_SCANCODE_C] || pad(SDL_GAMEPAD_BUTTON_WEST) || bound(Button::three);
    if (live && b1) in0 &= ~unsigned(in0_bits[0]);
    if (live && b2) in0 &= ~unsigned(in0_bits[1]);
    if (live && b3) in0 &= ~unsigned(in0_bits[2]);
    if (test_switch) in0 &= ~0x8000u;
    in.in0 = static_cast<std::uint16_t>(in0);
    // IN1, active low: the coins and the service coin (the low bits are the board's own).
    unsigned in1 = 0xf0;
    if (live && (key[SDL_SCANCODE_5] || pad(SDL_GAMEPAD_BUTTON_BACK) || bound(Button::coin_left))) in1 &= ~unsigned(in1_bits[0]);
    if (live && (key[SDL_SCANCODE_6] || bound(Button::coin_right))) in1 &= ~unsigned(in1_bits[1]);
    if (live && (key[SDL_SCANCODE_9] || bound(Button::service))) in1 &= ~unsigned(in1_bits[2]);
    in.in1 = static_cast<std::uint8_t>(in1);

    // The gas pedal, ADC1. A bound pedal axis gives its travel; otherwise the
    // Up key and gamepad triggers move a pedal 8 steps a frame, as a foot would.
    double travel = 0;
    if (!settings.pedal_axis.empty() && find(settings.pedal_axis.device)) {
        const Device* d = find(settings.pedal_axis.device);
        travel = controls::pedal_travel(axis_of(d->joystick, settings.pedal_axis.index), settings.pedal);
    }
    int target = key[SDL_SCANCODE_UP] ? 0x3f : 0;
    for (const auto& d : devices_)
        if (defaults_from(d)) target = std::max(target, SDL_GetGamepadAxis(d.gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 0x3f / 32767);
    pedal_ = target > pedal_ ? std::min(target, pedal_ + 8) : std::max(target, pedal_ - 8);
    travel = std::max(travel, pedal_ / 63.0);
    if (!live) travel = 0;
    last_pedal = travel;
    in.adc[1] = controls::pedal_adc(travel);

    // The steering wheel: a free-spinning dial on LETA0, counts a frame.
    // The arrow keys ask for 60% of the full speed; else the stick pushed furthest.
    const double keys = (key[SDL_SCANCODE_RIGHT] ? 0.6 : 0) - (key[SDL_SCANCODE_LEFT] ? 0.6 : 0);
    double stick = 0;
    for (const auto& d : devices_)
        if (defaults_from(d)) {
            const double x = SDL_GetGamepadAxis(d.gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0;
            if (std::abs(x) > std::abs(stick)) stick = x;
        }
    int counts = keys != 0 ? stick_.update_linear(keys) : stick_.update(stick);
    if (settings.steering == Steering::wheel && !settings.wheel_axis.empty()) {
        if (const Device* d = find(settings.wheel_axis.device)) counts += wheel_.update(axis_of(d->joystick, settings.wheel_axis.index));
        else wheel_.reset();   // unplugged: when it's back, its first reading only says where it is
    }
    if (settings.steering == Steering::mouse) {
        mouse_carry_ += mouse_dx_ * settings.mouse_counts;
        const double whole = std::trunc(mouse_carry_);
        mouse_carry_ -= whole;
        counts += static_cast<int>(whole);
    }
    mouse_dx_ = 0;
    if (!live) counts = 0;
    last_counts = counts;
    in.leta[0] = static_cast<std::uint8_t>(in.leta[0] + counts);
}

}  // namespace qilin::app
