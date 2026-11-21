#include "system2_speech.hpp"

namespace qilin::sys2 {

Speech::Speech() : tms5220_core(VARIANT_5220C) {
    device_start();
    device_reset();
}

void Speech::timer_io_ready_clocks(std::int32_t clocks, std::int32_t param) {
    if (external_io_ready_timer) return external_io_ready_timer(clocks, 0, param);
    timer_at_ = now_ + clocks * cycles_per_clock_;   // MAME's timer adjust: replaces any pending one
    timer_param_ = param;
}

void Speech::timer_io_ready_usec(std::int32_t usec, std::int32_t param) {
    if (external_io_ready_timer) return external_io_ready_timer(-1, usec, param);
    timer_at_ = now_ + usec * 10;   // 10 main cycles a microsecond
    timer_param_ = param;
}

void Speech::fire_io_ready(std::int32_t param, std::int64_t at) {
    advance(at);
    set_io_ready(param);
}

// Samples start every 80 chip clocks; the timer fires at its time, after
// the samples that start before it, as MAME's timer callback updates the
// stream first.
void Speech::advance(std::int64_t at) {
    if (mame_samples_due) {
        // The io-ready timer is the caller's (external_io_ready_timer).
        for (const auto due = mame_samples_due(); samples_ < due;) {
            std::int16_t sample = 0;
            process(&sample, 1);
            if (sample > 64 || sample < -64) ++loud_;
            if (on_sample) on_sample(sample, mame_sample_start(samples_));
            ++samples_;
        }
        now_ = at;
        return;
    }
    // The 6502 runs behind the T-11, so the main CPU's accesses (its sound
    // reset) can come at a time a little before the 6502's: never go back.
    if (at < now_) at = now_;
    for (;;) {
        const bool timer_first = timer_at_ >= 0 && timer_at_ <= at && timer_at_ <= next_sample_;
        if (timer_first) {
            now_ = timer_at_;
            timer_at_ = -1;
            set_io_ready(timer_param_);
            continue;
        }
        if (next_sample_ >= at) break;
        std::int16_t sample = 0;
        process(&sample, 1);
        ++samples_;
        if (sample > 64 || sample < -64) ++loud_;
        if (on_sample) on_sample(sample, next_sample_);
        next_sample_ += 80 * cycles_per_clock_;
    }
    now_ = at;
}

void Speech::data_w(std::uint8_t value, std::int64_t at) {
    advance(at);
    tms5220_core::data_w(value);
}

void Speech::wsq_w(int state, std::int64_t at) {
    advance(at);
    tms5220_core::wsq_w(state);
}

bool Speech::not_ready(std::int64_t at) {
    advance(at);
    return readyq_r() != 0;
}

void Speech::set_divider(int divider, std::int64_t at) {
    advance(at);
    cycles_per_clock_ = 4 * divider;   // 20 MHz / 4 / divider / 2 against 10 MHz
}

void Speech::reset(std::int64_t at) {
    advance(at);
    tms5220_core::reset();
}

} // namespace qilin::sys2
