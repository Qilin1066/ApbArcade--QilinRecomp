#pragma once
// The cabinet's controls from whatever the player has: the pure arithmetic,
// with no SDL in it, so it can be tested.
//
// APB's wheel is a free-spinning optical encoder read by the LETA chip as a
// count that wraps (MAME's IPT_DIAL): the game sees how far the wheel turned,
// never where it is. A PC wheel reports where it is, as an axis across its
// rotation range. So the dial is driven by how far the axis moves each frame,
// scaled to counts per turn of the wheel. On a direct-drive wheel with no
// stop (a Moza R5 with its range opened up) that is the whole story. A wheel
// that stops at a lock (a Logitech G920 at 900 degrees) can't go on turning,
// so past an edge near each lock the dial keeps turning on its own while
// the wheel is held there, faster the harder into the lock: the player turns
// to the lock and holds it to keep spinning, and comes back off it to stop.
// Sticks and keys turn the dial at a rate.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace qilin::controls {

struct WheelSettings {
    double rotation = 900;          // degrees lock to lock the axis spans, as the wheel's driver is set
    double counts_per_turn = 160;   // dial counts for one full turn of the wheel
    double edge = 0.92;             // past this fraction of the way to a lock, holding turns the dial on its own...
    double edge_rate = 6;           // ...at up to this many counts a frame, at the lock
    bool invert = false;
};

// One wheel axis to dial counts, a frame at a time.
class Wheel {
public:
    explicit Wheel(const WheelSettings& settings = {}) : s_(settings) {}
    void configure(const WheelSettings& settings) { s_ = settings; }
    void reset() { last_.reset(); carry_ = 0; }

    // 'axis' is the wheel's position, -1 (left lock) to 1 (right lock).
    // Returns the counts to add to the dial this frame (positive: right).
    int update(double axis) {
        axis = std::clamp(s_.invert ? -axis : axis, -1.0, 1.0);
        double counts = 0;
        if (last_) counts += (axis - *last_) * 0.5 * s_.rotation / 360.0 * s_.counts_per_turn;
        last_ = axis;
        if (s_.edge < 1 && std::abs(axis) > s_.edge) {
            // edge < 1 here; the floor only keeps MSVC (C4723) from seeing a 0 divisor where a test sets edge to 1.
            const double into = (std::abs(axis) - s_.edge) / std::max(1 - s_.edge, 1e-9);
            counts += std::copysign(s_.edge_rate * std::min(into, 1.0), axis);
        }
        return take(counts);
    }

private:
    int take(double counts) {
        carry_ += counts;
        const double whole = std::trunc(carry_);
        carry_ -= whole;
        return static_cast<int>(whole);
    }
    WheelSettings s_;
    std::optional<double> last_;
    double carry_ = 0;   // fractions of a count, kept so slow turns aren't lost
};

// A stick or keys: a rate, counts a frame at full deflection. Past the
// dead zone, a stick's deflection is raised to the power 'curve' (1:
// straight; 2, the default: a quarter of the speed at half way), so small
// pushes make fine corrections and a full push still turns fast. Keys,
// which are only ever on or off, ask for a fraction of the speed directly.
class Rate {
public:
    double deadzone = 0.15;
    double counts_at_full = 3;
    double curve = 2;
    // A stick's deflection, -1..1.
    int update(double x) {
        x = std::clamp(x, -1.0, 1.0);
        const double a = std::abs(x);
        return add(a > deadzone ? std::copysign(std::pow((a - deadzone) / (1 - deadzone), curve), x) : 0.0);
    }
    // A fraction of the full speed, -1..1, as it is (keys).
    int update_linear(double fraction) { return add(std::clamp(fraction, -1.0, 1.0)); }

private:
    int add(double fraction) {
        carry_ += fraction * counts_at_full;
        // A whole count less a rounding error (0.6 x 3 is 1.7999...) counts as whole.
        const double whole = std::trunc(carry_ + std::copysign(1e-9, carry_));
        carry_ -= whole;
        return static_cast<int>(whole);
    }

private:
    double carry_ = 0;
};

// A pedal axis, calibrated by the two readings it gives released and
// floored (pedals differ: some rest at -1, some at 0 or 1, some reversed).
struct PedalSettings {
    double released = -1;
    double floored = 1;
    double deadzone = 0.04;   // of the travel, at each end
};

// How far down the pedal is, 0 to 1.
inline double pedal_travel(double axis, const PedalSettings& p) {
    if (p.floored == p.released) return 0;
    const double t = std::clamp((axis - p.released) / (p.floored - p.released), 0.0, 1.0);
    if (t <= p.deadzone) return 0;
    if (t >= 1 - p.deadzone) return 1;
    return (t - p.deadzone) / (1 - 2 * p.deadzone);
}

// APB's gas pedal on ADC1: 0x3F released, 0 floored (MAME's PORT_INVERT).
inline std::uint8_t pedal_adc(double travel) {
    return static_cast<std::uint8_t>(0x3f - std::lround(std::clamp(travel, 0.0, 1.0) * 0x3f));
}

} // namespace qilin::controls
