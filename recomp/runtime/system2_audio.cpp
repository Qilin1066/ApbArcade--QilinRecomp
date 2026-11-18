#include "system2_audio.hpp"

#include "ymfm_opm.h"

#include <algorithm>
#include <cmath>

namespace qilin::sys2 {

namespace {

// AUDC and AUDCTL bits (pokey.cpp).
constexpr std::uint8_t NOTPOLY5 = 0x80, POLY4 = 0x40, PURE = 0x20, VOLUME_ONLY = 0x10, VOLUME_MASK = 0x0f;
constexpr std::uint8_t POLY9 = 0x80, CH1_HICLK = 0x40, CH3_HICLK = 0x20, CH12_JOINED = 0x10, CH34_JOINED = 0x08,
                       CH1_FILTER = 0x04, CH2_FILTER = 0x02, CLK_15KHZ = 0x01;
constexpr std::uint8_t SK_RESET = 0x03, SK_TWOTONE = 0x08, SK_ASYNC = 0x10;
constexpr int DIV_64 = 28, DIV_15 = 114;
constexpr int CLK_28 = 1, CLK_114 = 2;   // clock_cnt_ slots (slot 0, the 1.79 MHz clock, never divides)

// The polynomial counters' outputs, as pokey.cpp makes them.
struct Polys {
    std::array<std::uint8_t, 15> p4{};
    std::array<std::uint8_t, 31> p5{};
    std::array<std::uint8_t, 511> p9{};
    std::vector<std::uint8_t> p17 = std::vector<std::uint8_t>(0x1ffff);
    Polys() {
        auto poly45 = [](auto& out, int size) {
            const unsigned mask = (1u << size) - 1;
            std::uint32_t lfsr = 0;
            const int xorbit = size - 1;
            for (unsigned i = 0; i < mask; ++i) {
                lfsr = (lfsr << 1) | (~((lfsr >> 2) ^ (lfsr >> xorbit)) & 1);
                out[i] = static_cast<std::uint8_t>(lfsr & mask & 1);
            }
        };
        poly45(p4, 4);
        poly45(p5, 5);
        std::uint32_t lfsr = 0x1ff;
        for (unsigned i = 0; i < 0x1ff; ++i) {
            const std::uint32_t in = (lfsr & 1) ^ ((lfsr >> 5) & 1);
            lfsr = (lfsr >> 1) | (in << 8);
            p9[i] = static_cast<std::uint8_t>(lfsr & 1);
        }
        lfsr = 0x1ffff;
        for (unsigned i = 0; i < 0x1ffff; ++i) {
            const std::uint32_t in8 = ((lfsr >> 8) & 1) ^ ((lfsr >> 13) & 1);
            const std::uint32_t in = lfsr & 1;
            lfsr >>= 1;
            lfsr = (lfsr & 0xff7f) | (in8 << 7);
            lfsr = (in << 16) | lfsr;
            p17[i] = static_cast<std::uint8_t>(lfsr & 1);
        }
    }
};
const Polys& polys() {
    static const Polys p;
    return p;
}

// MAME's mixer_w: a volume bit pulls a resistor to ground below a 50k top.
double gain(std::uint8_t latch, int first, std::initializer_list<double> resistors) {
    const double top = 1.0 / (1.0 / 100 + 1.0 / 100);
    double bottom = 0;
    int bit = first;
    for (double r : resistors) {
        if (!((latch >> bit) & 1)) bottom += 1.0 / r;
        ++bit;
    }
    return bottom == 0 ? 1.0 : (1.0 / bottom) / (top + 1.0 / bottom);
}

} // namespace

// ---- POKEY ------------------------------------------------------------------

PokeySound::PokeySound() { (void)polys(); }

void PokeySound::write(unsigned reg, std::uint8_t value) {
    switch (reg & 15) {
    case 0: case 2: case 4: case 6: ch_[reg >> 1].audf = value; break;
    case 1: case 3: case 5: case 7: ch_[reg >> 1].audc = value; dirty_ = true; break;
    case 8:
        if (value == audctl_) return;
        audctl_ = value;
        dirty_ = true;
        break;
    case 9:   // STIMER: every counter restarts
        for (int i = 0; i < 4; ++i) {
            ch_[i].reset();
            ch_[i].output = 0;
            ch_[i].filter_sample = i < 2 ? 1 : 0;
        }
        dirty_ = true;
        break;
    case 15:   // SKCTL
        if (value == skctl_) return;
        skctl_ = value;
        if (!(value & SK_RESET)) {
            p4_ = p5_ = p9_ = p17_ = 0;
            clock_cnt_[0] = clock_cnt_[1] = clock_cnt_[2] = 0;
        }
        // The serial input is idle on System 2, so asynchronous mode resets channels 3 and 4.
        if (value & SK_ASYNC) {
            ch_[2].reset();
            ch_[3].reset();
        }
        dirty_ = true;
        break;
    default: break;   // POTGO, SEROUT, IRQEN, SKREST: nothing audible
    }
}

void PokeySound::process(int c) {
    const Polys& p = polys();
    Channel& ch = ch_[c];
    if ((ch.audc & NOTPOLY5) || p.p5[p5_]) {
        if (ch.audc & PURE) ch.output ^= 1;
        else if (ch.audc & POLY4) ch.output = p.p4[p4_];
        else if (audctl_ & POLY9) ch.output = p.p9[p9_];
        else ch.output = p.p17[p17_];
        dirty_ = true;
    }
}

// pokey_device::step_one_clock, less the serial port, pots and keyboard,
// which System 2 doesn't use for sound. Its serial input is idle, so
// asynchronous receive mode holds channels 3 and 4 in reset.
void PokeySound::step() {
    const bool async_reset = (skctl_ & SK_ASYNC) != 0;
    if (skctl_ & SK_RESET) {
        if (++p4_ == 0x0000f) p4_ = 0;
        if (++p5_ == 0x0001f) p5_ = 0;
        if (++p9_ == 0x001ff) p9_ = 0;
        if (++p17_ == 0x1ffff) p17_ = 0;
        bool triggered[3] = {true, false, false};
        if (++clock_cnt_[CLK_28] >= DIV_64) {
            clock_cnt_[CLK_28] = 0;
            triggered[CLK_28] = true;
        }
        if (++clock_cnt_[CLK_114] >= DIV_15) {
            clock_cnt_[CLK_114] = 0;
            triggered[CLK_114] = true;
        }
        const int base = (audctl_ & CLK_15KHZ) ? CLK_114 : CLK_28;
        if (audctl_ & CH1_HICLK) ch_[0].inc((audctl_ & CH12_JOINED) ? 7 : 4);
        else if (triggered[base]) ch_[0].inc(1);
        if (!async_reset) {
            if (audctl_ & CH3_HICLK) ch_[2].inc((audctl_ & CH34_JOINED) ? 7 : 4);
            else if (triggered[base]) ch_[2].inc(1);
        }
        if (triggered[base]) {
            if (!(audctl_ & CH12_JOINED)) ch_[1].inc(1);
            if (!(audctl_ & CH34_JOINED) && !async_reset) ch_[3].inc(1);
        }
    }
    if (ch_[2].check_borrow()) {
        if (audctl_ & CH34_JOINED) ch_[3].inc(1);
        else ch_[2].reset();
        process(2);
        if (audctl_ & CH1_FILTER) ch_[0].filter_sample = ch_[0].output;
        else ch_[0].filter_sample = 1;
        dirty_ = true;
    }
    if (ch_[3].check_borrow()) {
        if (audctl_ & CH34_JOINED) ch_[2].reset();
        ch_[3].reset();
        process(3);
        if (audctl_ & CH2_FILTER) ch_[1].filter_sample = ch_[1].output;
        else ch_[1].filter_sample = 1;
        dirty_ = true;
    }
    if ((skctl_ & SK_TWOTONE) && ch_[1].borrow == 1) {
        ch_[0].reset();
        dirty_ = true;
    }
    if (ch_[0].check_borrow()) {
        if (audctl_ & CH12_JOINED) {
            ch_[1].inc(1);
        } else {
            ch_[0].reset();
            // Two-tone mode with the serial output idle (System 2 never sends): its level is the inverse of BREAK.
            if ((skctl_ & SK_TWOTONE) && !(skctl_ & 0x80)) {
                ch_[1].reset();
                dirty_ = true;
            }
        }
        process(0);
    }
    if (ch_[1].check_borrow()) {
        if (audctl_ & CH12_JOINED) ch_[0].reset();
        ch_[1].reset();
        process(1);
    }
    if (dirty_) {
        unsigned sum = 0;
        for (const Channel& ch : ch_)
            if ((ch.output ^ ch.filter_sample) || (ch.audc & VOLUME_ONLY)) sum += ch.audc & VOLUME_MASK;
        out_ = sum;
        dirty_ = false;
    }
}

std::uint32_t PokeySound::run(int n) {
    constexpr std::uint32_t gain = 32767 / 11 / 4;   // POKEY_DEFAULT_GAIN
    auto level = [&] { return std::min(out_ * gain, 32767u); };
    std::uint32_t sum = 0;
    // Silent: no channel has a volume or forces its output. APB keeps its
    // POKEYs like this (every register 0, AUDCTL 0x78: two 16-bit channels
    // at 1.79 MHz, borrowing every 7 cycles), which costs a step per cycle
    // to follow. Nothing can be heard until a register changes, so the
    // channels are left where they are; only the polynomial counters and the
    // clock dividers move on. A channel that becomes audible starts from the
    // counter it stopped at, a difference in phase only.
    if (std::ranges::all_of(ch_, [](const Channel& c) { return (c.audc & (VOLUME_ONLY | VOLUME_MASK)) == 0; })) {
        if (skctl_ & SK_RESET) {
            const auto q = static_cast<std::uint32_t>(n);
            p4_ = (p4_ + q) % 0x0f;
            p5_ = (p5_ + q) % 0x1f;
            p9_ = (p9_ + q) % 0x1ff;
            p17_ = (p17_ + q) % 0x1ffff;
            clock_cnt_[CLK_28] = (clock_cnt_[CLK_28] + n) % DIV_64;
            clock_cnt_[CLK_114] = (clock_cnt_[CLK_114] + n) % DIV_15;
        }
        out_ = 0;
        dirty_ = false;
        return 0;
    }
    while (n > 0) {
        const bool pending = ch_[0].borrow || ch_[1].borrow || ch_[2].borrow || ch_[3].borrow || dirty_;
        if (!pending) {
            // Steps until something can happen: a tick of the base clock, or
            // a fast-clocked channel's counter wrapping. Before that only the
            // polynomial counters, the clock dividers and those counters move.
            int quiet = n;
            const bool running = (skctl_ & SK_RESET) != 0;
            const bool hi1 = running && (audctl_ & CH1_HICLK);
            const bool hi3 = running && (audctl_ & CH3_HICLK) && !(skctl_ & SK_ASYNC);
            if (running) {
                const int base = (audctl_ & CLK_15KHZ) ? CLK_114 : CLK_28;
                const int divisor = base == CLK_114 ? DIV_15 : DIV_64;
                quiet = std::min(quiet, divisor - clock_cnt_[base] - 1);
                if (hi1) quiet = std::min(quiet, 255 - ch_[0].counter);
                if (hi3) quiet = std::min(quiet, 255 - ch_[2].counter);
            }
            if (quiet > 0) {
                if (running) {
                    const auto q = static_cast<std::uint32_t>(quiet);
                    p4_ = (p4_ + q) % 0x0f;
                    p5_ = (p5_ + q) % 0x1f;
                    p9_ = (p9_ + q) % 0x1ff;
                    p17_ = (p17_ + q) % 0x1ffff;
                    clock_cnt_[CLK_28] = (clock_cnt_[CLK_28] + quiet) % DIV_64;
                    clock_cnt_[CLK_114] = (clock_cnt_[CLK_114] + quiet) % DIV_15;
                    if (hi1) ch_[0].counter += quiet;
                    if (hi3) ch_[2].counter += quiet;
                }
                sum += level() * static_cast<std::uint32_t>(quiet);
                n -= quiet;
                continue;
            }
        }
        step();
        sum += level();
        --n;
    }
    return sum;
}

// ---- The board's sound ------------------------------------------------------

struct Audio::Ym {
    ymfm::ymfm_interface interface;   // timers and IRQ unused: System 2 doesn't wire the YM2151's IRQ
    ymfm::ym2151 chip{interface};
};

Audio::Audio() : ym_(std::make_unique<Ym>()) {
    ym_->chip.reset();
    frames_.reserve(4096);
}

Audio::~Audio() = default;

void Audio::pokey_write(int which, unsigned reg, std::uint8_t value, std::int64_t sound_time) {
    advance(sound_time);
    pokey_[static_cast<std::size_t>(which)].write(reg, value);
}

void Audio::ym_write(bool data, std::uint8_t value, std::int64_t sound_time) {
    advance(sound_time);
    if (!ym_running_) return;   // held in reset: MAME's ym2151_device ignores writes
    if (data) ym_->chip.write_data(value);
    else ym_->chip.write_address(value);
}

void Audio::ym_reset(bool running, std::int64_t sound_time) {
    advance(sound_time);
    if (running == ym_running_) return;
    ym_running_ = running;
    if (running) ym_->chip.reset();
}

void Audio::mixer_write(std::uint8_t value, std::int64_t sound_time) {
    advance(sound_time);
    ym_gain_ = gain(value, 0, {100, 47, 22});
    pokey_gain_ = gain(value, 3, {47, 22});
}

void Audio::advance(std::int64_t sound_time) {
    while (time_ < sound_time) {
        const auto into = static_cast<int>(time_ % cycles_per_sample);
        const auto n = static_cast<int>(std::min<std::int64_t>(cycles_per_sample - into, sound_time - time_));
        pokey_sum_[0] += pokey_[0].run(n);
        pokey_sum_[1] += pokey_[1].run(n);
        time_ += n;
        if (time_ % cycles_per_sample == 0) emit();
    }
}

void Audio::emit() {
    ymfm::ym2151::output_data out;
    ym_->chip.generate(&out, 1);
    // MAME's levels, as fractions of full scale: the YM2151's 16-bit
    // outputs, and each POKEY's output averaged over the frame's cycles.
    constexpr double pokey_scale = 1.0 / 32768.0 / cycles_per_sample;
    const double pokey[2] = {pokey_sum_[0] * pokey_scale, pokey_sum_[1] * pokey_scale};
    pokey_sum_ = {};
    for (int side = 0; side < 2; ++side) {
        const double v = out.data[side] / 32768.0 * 0.60 * ym_gain_ + pokey[side] * 1.35 * pokey_gain_;
        frames_.push_back(static_cast<std::int16_t>(std::clamp(std::lround(v * 32767.0), -32768L, 32767L)));
    }
}

} // namespace qilin::sys2
