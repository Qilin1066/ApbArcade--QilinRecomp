#pragma once
// System 2 sound, as MAME's atarisy2.cpp makes it: the YM2151 (ymfm, the
// library MAME uses, vendored unmodified in third_party/ymfm), two POKEYs
// (a port of the sound side of MAME's pokey.cpp: channels, polynomial
// counters, high-pass filters, joined and fast-clocked channels, its linear
// output), and MAME's mixer: the YM2151 at 0.60 into both sides, POKEY 1 left
// and POKEY 2 right at 1.35, each scaled by the volume latch's resistor
// network.
//
// Time is the 6502's clock, which is also the POKEYs' (14.318181 MHz / 8).
// The YM2151 runs at 14.318181 MHz / 4 and makes a sample every 64 of its
// clocks: every 32 sound cycles, 55,930.4 a second. That is the output rate;
// each output frame averages the POKEYs over its 32 cycles.
//
// Writes come from the board with the sound cycle they happen on (the
// machine's sound sink); the sound is made up to that cycle first, so a
// write takes effect exactly when the 6502 made it, as MAME's stream update
// before a write does. The TMS5220's speech (the board runs the chip; see
// system2_speech.hpp) arrives as samples at 7.8 or 10.4 kHz with the main
// cycle each starts on, and is interpolated to the output rate: at 0.75
// into both sides, scaled by the volume latch's bits 5-7.

#include "system2_machine.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace qilin::sys2 {

// One POKEY's sound generator.
class PokeySound {
public:
    PokeySound();
    void write(unsigned reg, std::uint8_t value);
    void step();                                  // one clock
    [[nodiscard]] unsigned output() const noexcept { return out_; }   // sum of the channels' volumes, 0-60
    // n clocks, the same as n steps; returns the sum over them of the
    // output's level (pokey.cpp's linear output: volumes x 744, clipped).
    // Between ticks of the 64 or 15 kHz clock and wraps of fast-clocked
    // counters nothing but counting happens, so those stretches are skipped
    // in one go. A POKEY with every channel silent isn't run (see run()).
    std::uint32_t run(int n);

private:
    struct Channel {
        std::uint8_t audf = 0, audc = 0xb0;
        std::int32_t borrow = 0, counter = 0;
        std::uint8_t output = 0, filter_sample = 0;
        void reset() noexcept { counter = audf ^ 0xff; borrow = 0; }
        void inc(int cycles) noexcept {
            counter = (counter + 1) & 0xff;
            if (counter == 0 && borrow == 0) borrow = cycles;
        }
        bool check_borrow() noexcept { return borrow > 0 && --borrow == 0; }
    };
    void process(int ch);

    std::array<Channel, 4> ch_{};
    std::uint8_t audctl_ = 0, skctl_ = 0;
    std::uint32_t p4_ = 0, p5_ = 0, p9_ = 0, p17_ = 0;
    std::int32_t clock_cnt_[3] = {0, 0, 0};
    bool dirty_ = true;
    unsigned out_ = 0;
};

class Audio final : public Machine::SoundSink {
public:
    static constexpr double sample_rate = 14318181.0 / 256;   // 55,930.4 Hz
    static constexpr int cycles_per_sample = 32;

    Audio();
    ~Audio() override;

    // Machine::SoundSink
    void pokey_write(int which, unsigned reg, std::uint8_t value, std::int64_t sound_time) override;
    void ym_write(bool data, std::uint8_t value, std::int64_t sound_time) override;
    void ym_reset(bool running, std::int64_t sound_time) override;
    void mixer_write(std::uint8_t value, std::int64_t sound_time) override;
    void speech_sample(std::int16_t value, std::int64_t main_cycle) override;

    // Makes the sound up to a sound cycle.
    void advance(std::int64_t sound_time);
    // Stereo frames made so far (left, right, ...), taken by the caller.
    std::vector<std::int16_t>& frames() noexcept { return frames_; }

private:
    void emit();

    struct Ym;                   // ymfm's chip, kept out of this header (its warnings stay its own)
    std::unique_ptr<Ym> ym_;
    bool ym_running_ = true;   // MAME's ym2151_device starts out of reset
    std::array<PokeySound, 2> pokey_;
    std::array<std::uint32_t, 2> pokey_sum_{};
    double ym_gain_ = 1.0, pokey_gain_ = 1.0, speech_gain_ = 1.0;
    struct SpeechSample {
        std::int64_t at;
        std::int16_t value;
    };
    SpeechSample speech_last_{0, 0};
    std::deque<SpeechSample> speech_;
    std::int64_t time_ = 0;
    std::vector<std::int16_t> frames_;
};

} // namespace qilin::sys2
