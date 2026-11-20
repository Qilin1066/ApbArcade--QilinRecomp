#pragma once
// The scripted controls shared by qilin_refrun, qilin_run and qilin_handoff:
// coins, button presses, the pedal, the wheel, the random player and the
// self-test switch. The controls change at the start of each vblank, on the
// exact cycle the frame count goes up (Machine::on_vblank), so a reference
// run and a native run see the same inputs at the same moments and their
// frames can be compared one for one.

#include "monkey.hpp"
#include "system2_machine.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qilin::tools {

class Script {
public:
    // Takes the option if it is one of the script's; false if it isn't.
    bool option(std::string_view flag, std::string_view value) {
        auto parse = [](std::string_view text, auto& parsed, int base = 10) {
            if (text.empty()) return false;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, base);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size();
        };
        auto invalid = [&] { error_ = "Invalid value for " + std::string(flag) + ": " + std::string(value); };
        std::uint64_t number = 0;
        if (flag == "--steer") {
            int signed_value = 0;
            if (!parse(value, signed_value) || signed_value < -128 || signed_value > 127) invalid();
            else steer_ = signed_value;
        } else if (flag == "--press") {
            const auto colon = value.find(':');
            unsigned mask = 0;
            if (colon == std::string_view::npos || !parse(value.substr(0, colon), mask, 16) || mask > 0xffff ||
                !parse(value.substr(colon + 1), number)) invalid();
            else presses_.emplace_back(static_cast<std::uint16_t>(mask), number);
        } else if (flag == "--coin" || flag == "--pedal" || flag == "--monkey" || flag == "--test-switch") {
            if (!parse(value, number)) invalid();
            else if (flag == "--coin") coins_.push_back(number);
            else if (flag == "--pedal") {
                if (number > 63) invalid(); else pedal_ = static_cast<int>(number);
            } else if (flag == "--monkey") monkey_.emplace(number);
            else if (number > 1) invalid();
            else test_switch_ = number != 0;
        } else return false;
        used_ = true;
        return true;
    }

    [[nodiscard]] bool used() const { return used_; }
    [[nodiscard]] const std::string& error() const { return error_; }

    // Drives the machine's inputs from power-on: frame 0 now, each later one
    // at the start of its vblank.
    void attach(sys2::Machine& machine) {
        apply(0, machine.inputs);
        drive(machine);
    }
    // Drives a machine that carries on from another's state (a copy also
    // copies the hook, still pointing at the original): from its next vblank.
    void drive(sys2::Machine& machine) {
        machine.on_vblank = [this, &machine](std::uint64_t frame) { apply(frame, machine.inputs); };
    }

    // The controls for frame 'frame' (frames completed so far); call once per frame, in order.
    void apply(std::uint64_t frame, sys2::Inputs& in) {
        const bool coin = std::ranges::any_of(coins_, [&](std::uint64_t f) { return frame >= f && frame - f < 3; });
        in.in1 = static_cast<std::uint8_t>(coin ? 0xb0 : 0xf0);
        std::uint16_t in0 = 0xffff;
        for (const auto& [mask, at] : presses_)
            if (frame >= at && frame - at < 10) in0 = static_cast<std::uint16_t>(in0 & ~mask);
        in.in0 = in0;
        if (pedal_ >= 0) in.adc[1] = static_cast<std::uint8_t>(pedal_);
        if (steer_ != 0 && frame > 0) in.leta[0] = static_cast<std::uint8_t>(in.leta[0] + steer_);
        if (monkey_) {
            sys2::Inputs chosen = in;
            monkey_->apply(frame, chosen);
            in.in0 = static_cast<std::uint16_t>(in.in0 & chosen.in0);
            if (chosen.in1 != 0xf0) in.in1 = chosen.in1;
            in.adc[1] = chosen.adc[1];
            in.leta[0] = chosen.leta[0];
        }
        // The self-test switch (IN0 bit 15, active low): held from power-on, the game runs its tests and menus.
        if (test_switch_) in.in0 = static_cast<std::uint16_t>(in.in0 & 0x7fff);
    }

private:
    bool used_ = false;
    std::string error_;
    std::vector<std::uint64_t> coins_;
    std::vector<std::pair<std::uint16_t, std::uint64_t>> presses_;
    int pedal_ = -1, steer_ = 0;
    std::optional<Monkey> monkey_;
    bool test_switch_ = false;
};

} // namespace qilin::tools
