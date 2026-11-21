#pragma once
// The System 2 speech chip: a TMS5220C, which is MAME's tms5220.cpp
// (third_party/mame-tms5220, adapted to run outside MAME's device framework,
// every line of its logic MAME's) on the board's clock.
//
// It's part of the board, not only of the sound: the 6502 reads its READY
// line (IN1 bit 2) and waits on it, and whether a write is taken depends on
// how much of the FIFO the chip has spoken. So it runs in main-CPU cycles,
// the board's time, and is brought up to date before every access, as MAME
// updates its stream first.
//
// Clock: 20 MHz / 4 / divider / 2, the divider 4 or 3 by switch_6502_w's
// bit 5 (625 or 833 kHz): 16 or 12 main cycles a chip clock, a sample every
// 80 chip clocks (7,812.5 or 10,416.7 Hz). APB's wiring: data latch at
// 0x1870, /WS by 0x1872 (high) and 0x1873 (low), no /RS, no IRQ, no speech
// ROM.

#include "tms5220_core.h"

#include <cstdint>
#include <functional>

namespace qilin::sys2 {

class Speech final : private tms5220_core {
public:
    Speech();

    // All at a time in main-CPU cycles; one earlier than the last counts as the last.
    void data_w(std::uint8_t value, std::int64_t at);
    void wsq_w(int state, std::int64_t at);
    [[nodiscard]] bool not_ready(std::int64_t at);       // /READY, as IN1 bit 2 reads it
    void set_divider(int divider, std::int64_t at);    // switch_6502_w
    void reset(std::int64_t at);                       // the chip's reset (device_reset)
    void advance(std::int64_t at);                     // makes the samples up to a time

    // Each sample as it's made, with the main-CPU cycle it starts on.
    std::function<void(std::int16_t, std::int64_t)> on_sample;

    [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }
    // The diagnostic MAME-scheduling mode: the io-ready timer becomes the
    // caller's (a MAME timer): given the delay in chip clocks (or, when
    // clocks < 0, in microseconds) and the parameter; it calls fire_io_ready.
    std::function<void(std::int32_t clocks, std::int32_t usec, std::int32_t param)> external_io_ready_timer;
    void fire_io_ready(std::int32_t param, std::int64_t at);
    // The same mode: MAME's stream decides which samples exist by a time
    // (how many are due, and the main cycle each starts on); 'at' is ignored.
    std::function<std::uint64_t()> mame_samples_due;
    std::function<std::int64_t(std::uint64_t index)> mame_sample_start;
    [[nodiscard]] std::uint32_t chip_hz() const noexcept { return cycles_per_clock_ == 16 ? 625'000u : 833'333u; }
    // Samples louder than the idle level (|value| > 64): the chip said something.
    [[nodiscard]] std::uint64_t loud_samples() const noexcept { return loud_; }

private:
    void stream_update() override { }   // the host brings it up to date before each call
    void timer_io_ready_clocks(std::int32_t clocks, std::int32_t param) override;
    void timer_io_ready_usec(std::int32_t usec, std::int32_t param) override;

    std::int64_t now_ = 0;             // the time of the call in progress
    std::int64_t cycles_per_clock_ = 16;
    std::int64_t next_sample_ = 0;     // when the next sample starts
    std::int64_t timer_at_ = -1;       // the io-ready timer, or -1
    std::int32_t timer_param_ = 0;
    std::uint64_t samples_ = 0, loud_ = 0;
};

} // namespace qilin::sys2
