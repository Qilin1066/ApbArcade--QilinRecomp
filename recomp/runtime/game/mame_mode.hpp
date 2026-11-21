#pragma once
// The diagnostic MAME-scheduling mode (qilin_run --mame-scheduling): the
// lifted T-11 code on the board, interleaved with the sound CPU exactly as
// standalone MAME 0.264 interleaves APB's CPUs, so a run can be compared
// with MAME's frame for frame and sample for sample.
//
// What it reproduces, after MAME's source (src/emu/schedule.cpp,
// diexec.cpp, screen.cpp, src/mame/atari/atarisy2.cpp and the devices):
//   - the scheduler: timeslices up to the next timer or the end of the
//     quantum, MAME's execute order (T-11, 6502, then the two POKEYs, which
//     MAME runs as executing devices too), timeslices aborted when a timer
//     goes to the head of the list, the 200 us perfect quantum after each
//     sound command, attotime arithmetic (mame/scheduler.hpp);
//   - every timer MAME runs for APB: the screen's vblank and line-0 timers,
//     the 32V scanline timer, the motion objects' forced updates, the 6502's
//     periodic IRQ (re-armed on each sound reset), the watchdog, the ADC0809,
//     the speech chip's io-ready timer, POKEY pot scanning, the sound
//     manager's 50 Hz update, the y-scroll load at line 0;
//   - everything MAME synchronises: set_input_line (queued, applied by a
//     timer), both latches' writes, the interrupt enable, POKEY reads and
//     writes, the YM2151's mode register and reset;
//   - the 6502 as MAME's own core (third_party/mame-m6502), because MAME
//     stops it on any bus cycle when its timeslice runs out and resumes it
//     there; the lifted 6502 runs whole instructions (it is checked against
//     the same core instruction by instruction elsewhere).
// The player never uses any of this.

#include "game.hpp"
#include "mame/scheduler.hpp"
#include "m6502_reference.h"
#include "system2_machine.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace qilin::game {

class MameRun final : public Scheduler::External, public sys2::MameHooks {
public:
    MameRun(sys2::Machine& machine, Scheduler& scheduler);
    ~MameRun() override;

    // The scheduler log, in the instrumented MAME's format (see docs/independent-mame.md).
    void set_log(std::FILE* file, double until_seconds) {
        ms_.log = file;
        ms_.log_until = until_seconds;
    }

    // Scheduler::External
    void start() override;
    void main_service() override;
    void main_wait() override;
    [[nodiscard]] std::int64_t main_now(std::int32_t icount) const override {
        return main_.clock_cycles + (main_.cycles_running - icount);
    }

    // sys2::MameHooks
    void update_interrupts() override;
    void int_enable_w(std::uint8_t data) override;
    void soundlatch_w(std::uint8_t data) override;
    void soundlatch_read() override;
    void mainlatch_w(std::uint8_t data) override;
    void sound_reset_line(bool asserted) override;
    void sound_irq_ack() override;
    void watchdog_reset() override;
    void adc_start(unsigned channel) override;
    [[nodiscard]] std::uint8_t adc_read() override { return adc_sar_; }
    void yscroll_reset(int scroll) override;
    [[nodiscard]] int vpos() override;
    [[nodiscard]] std::uint8_t pokey_read(int which, unsigned reg) override;
    void pokey_write(int which, unsigned reg, std::uint8_t value) override;
    [[nodiscard]] bool ym_busy() override;
    void ym_data_written() override;
    [[nodiscard]] std::int64_t speech_time() override;
    void ym_reset_line(bool running) override;

private:
    struct SoundBus final : m6502_device::Bus {
        MameRun* run = nullptr;
        void now();
        std::uint8_t read(std::uint16_t a) override;
        std::uint8_t read_sync(std::uint16_t a) override;
        std::uint8_t read_arg(std::uint16_t a) override;
        void write(std::uint16_t a, std::uint8_t v) override;
    };
    struct Pokey {
        std::int32_t icount = 0;
        std::uint8_t skctl = 0, audctl = 0, allpot = 0, pot_counter = 0;
        std::array<std::uint8_t, 8> potx{};
        unsigned p4 = 0, p5 = 0, p9 = 0, p17 = 0;
        std::array<int, 3> clock_cnt{};
    };

    void begin_main_run();
    void end_main_run();
    void check_stop();
    void pokey_run(int which);
    void pokey_step_pot(int which);
    void pokey_write_internal(int which, unsigned reg, std::uint8_t value);
    void set_main_line(int line, bool asserted);
    [[nodiscard]] mame::Time time_until_pos(int vpos) const;
    [[nodiscard]] std::int64_t main_cycles_at(mame::Time t) const;
    [[nodiscard]] std::int64_t sound_cycles_at(mame::Time t) const;
    void scanline_tick(std::int32_t);
    void force_update(std::int32_t param);
    void vblank_begin(std::int32_t);
    void vblank_end(std::int32_t);
    void first_scanline_tick(std::int32_t);
    void adc_update(std::int32_t);

    sys2::Machine& m_;
    Scheduler& s_;
    mame::Scheduler ms_;
    mame::Exec main_, audio_;
    std::array<mame::Exec, 2> pokey_exec_;
    std::array<Pokey, 2> pokey_;
    SoundBus bus_;
    m6502_device cpu6502_{bus_};

    mame::Timer *vblank_begin_ = nullptr, *vblank_end_ = nullptr, *scanline0_ = nullptr, *scantimer_ = nullptr,
                *force_update_ = nullptr, *watchdog_ = nullptr, *adc_timer_ = nullptr, *yscroll_timer_ = nullptr,
                *io_ready_ = nullptr, *sound_update_ = nullptr, *autoboot_ = nullptr;
    bool scantimer_first_ = true;
    mame::Time vblank_start_time_, vblank_end_time_;
    bool watchdog_fired_ = false;

    // The ADC0809 (adc0808.cpp).
    enum class Adc { idle, start, ready, running } adc_state_ = Adc::idle;
    std::uint8_t adc_sar_ = 0xff, adc_address_ = 0;
    bool adc_start_line_ = false;

    mame::Time ym_busy_end_;
    bool ym_reset_state_ = true;   // ym2151_device's m_reset_state: 1, running
};

} // namespace qilin::game
