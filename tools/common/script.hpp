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
#include <string_view>
#include <utility>
#include <vector>

namespace qilin::tools {

class Script {
public:
    // Takes the option if it is one of the script's; false if it isn't.
    bool option(std::string_view flag, std::string_view value) {
        std::uint64_t number = 0;
        std::from_chars(value.data(), value.data() + value.size(), number);
        if (flag == "--coin") coins_.push_back(number);
        else if (flag == "--pedal") pedal_ = static_cast<int>(number);
        else if (flag == "--steer") steer_ = static_cast<int>(number);
        else if (flag == "--monkey") monkey_.emplace(number);
        else if (flag == "--test-switch") test_switch_ = number != 0;
        else if (flag == "--press") {
            // MASK:FRAME, the mask in hex: IN0 bits held low for 10 frames.
            const auto colon = value.find(':');
            unsigned mask = 0;
            std::uint64_t frame = 0;
            std::from_chars(value.data(), value.data() + std::min(colon, value.size()), mask, 16);
            if (colon != std::string_view::npos) std::from_chars(value.data() + colon + 1, value.data() + value.size(), frame);
            presses_.emplace_back(static_cast<std::uint16_t>(mask), frame);
        } else {
            return false;
        }
        return true;
    }

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
        const bool coin = std::ranges::any_of(coins_, [&](std::uint64_t f) { return frame >= f && frame < f + 3; });
        in.in1 = static_cast<std::uint8_t>(coin ? 0xb0 : 0xf0);
        std::uint16_t in0 = 0xffff;
        for (const auto& [mask, at] : presses_)
            if (frame >= at && frame < at + 10) in0 = static_cast<std::uint16_t>(in0 & ~mask);
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
    std::vector<std::uint64_t> coins_;
    std::vector<std::pair<std::uint16_t, std::uint64_t>> presses_;
    int pedal_ = -1, steer_ = 0;
    std::optional<Monkey> monkey_;
    bool test_switch_ = false;
};

} // namespace qilin::tools
