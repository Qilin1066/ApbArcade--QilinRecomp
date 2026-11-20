#pragma once
// --monkey SEED: a player who doesn't know the game, for exploring it. Every
// frame it decides the controls from a seeded generator, so a run is
// repeatable: coins now and then, buttons held for random stretches, the
// pedal pressed and let off, the wheel turned at varying rates. Long runs of
// it reach parts of the game the scripted runs don't (more days, arrests,
// crashes, continues, the high-score table), and any code there that the
// analysis missed shows up in qilin_refrun as seeds, or in qilin_run as
// unlifted code.

#include "system2_machine.hpp"

#include <cstdint>

namespace qilin::tools {

class Monkey {
public:
    explicit Monkey(std::uint64_t seed) : state_(seed * 0x9e3779b97f4a7c15ull + 1) {}

    // The controls for a frame; call once per frame, in order.
    void apply(std::uint64_t frame, sys2::Inputs& in) {
        // A coin every 40-80 s, held 3 frames (a new credit or a continue).
        if (frame >= next_coin_) {
            coin_until_ = frame + 3;
            next_coin_ = frame + 2400 + next() % 2400;
        }
        // The app's siren/start, fire and alternate siren bindings, each
        // pressed or released for 5-90 frames (APB leaves IN0 bit 7 unused).
        static constexpr std::uint16_t masks[3] = {0x0008, 0x0002, 0x0008};
        for (int b = 0; b < 3; ++b) {
            if (frame >= button_until_[b]) {
                button_down_[b] = next() % 3 == 0;
                button_until_[b] = frame + 5 + next() % 85;
            }
        }
        // The pedal: a new position every 1-4 s, mostly pressed (ADC1: 0x3F up, 0 floored).
        if (frame >= pedal_until_) {
            pedal_ = next() % 4 == 0 ? 0x3f : static_cast<int>(next() % 40);
            pedal_until_ = frame + 60 + next() % 180;
        }
        // The wheel: a new rate every 0.2-1.5 s, -5 to +5 counts a frame.
        if (frame >= steer_until_) {
            steer_ = static_cast<int>(next() % 11) - 5;
            steer_until_ = frame + 12 + next() % 78;
        }
        unsigned in0 = 0xffff;
        for (int b = 0; b < 3; ++b)
            if (button_down_[b]) in0 &= ~static_cast<unsigned>(masks[b]);
        in.in0 = static_cast<std::uint16_t>(in0);
        in.in1 = static_cast<std::uint8_t>(frame < coin_until_ ? 0xb0 : 0xf0);
        in.adc[1] = static_cast<std::uint8_t>(pedal_);
        in.leta[0] = static_cast<std::uint8_t>(in.leta[0] + steer_);
    }

private:
    std::uint64_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }
    std::uint64_t state_;
    std::uint64_t next_coin_ = 900, coin_until_ = 0;
    std::uint64_t button_until_[3] = {0, 0, 0};
    bool button_down_[3] = {false, false, false};
    std::uint64_t pedal_until_ = 0, steer_until_ = 0;
    int pedal_ = 0x3f, steer_ = 0;
};

} // namespace qilin::tools
