#pragma once
// The game's scheduler: both CPUs' lifted code on one System 2 machine.
//
// The main CPU leads. Its lifted code runs until a service point finds its
// cycles used up; the scheduler then
//   1. advances the machine's clock by the cycles the T-11 used,
//   2. runs the sound CPU's lifted code up to the same time, in a slice it
//      ends by setting the sound board's 'yielding' (the 6502's interrupts are
//      taken at the start of each slice),
//   3. runs the board's events that fell due (scanline and VBLANK
//      interrupts, the sound IRQ, the watchdog),
//   4. takes a T-11 interrupt if one is pending above its priority, running
//      the handler natively, as MAME takes them at the start of a timeslice,
//   5. gives the T-11 its next slice: until the next event, at most
//      'quantum' cycles.
// Interrupt sequences make MAME's bus accesses and charge MAME's cycles
// (the T-11's 114; the 6502's 7, its reset's 8).
//
// Stopping (the frame count reached, the watchdog, dispatch finding no
// lifted code) throws game::Stop out of the lifted code to run().

#include "qilin_board.hpp"
#include "qilin_sound_board.hpp"
#include "system2_machine.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace qilin::game {

struct Stop {
    enum Reason { frames, watchdog, unlifted_main, unlifted_sound } reason;
    std::string detail;
};

class Scheduler {
public:
    explicit Scheduler(sys2::Machine& machine);

    // Powers on and runs until a Stop; returns it.
    Stop run(std::uint64_t frames);

    rt::Board main;
    rt6502::Board sound;
    std::int32_t quantum = 64;   // most main-CPU cycles between services
    // Called once per frame, at VBLANK, with the frame number: inputs go here.
    std::function<void(std::uint64_t)> on_frame;

    struct Counters {
        std::uint64_t main_interrupts = 0, sound_irqs = 0, sound_nmis = 0, sound_resets = 0, main_services = 0;
    } counters;

    // Called by the boards.
    void main_service();
    void main_wait();
    void sound_service();
    [[noreturn]] void unlifted_main();
    [[noreturn]] void unlifted_sound();

private:
    void account_main();
    void account_sound();
    void run_sound();
    bool take_main_interrupt();
    void take_sound_interrupt(bool nmi);
    void next_main_slice();

    sys2::Machine& m_;
    std::int32_t main_given_ = 0;     // icount at the start of the main CPU's slice
    std::int32_t sound_given_ = 0;
    std::int64_t sound_target_ = 0;   // sound cycles the current catch-up runs to
    bool sound_held_ = true;
    std::uint64_t nmi_edges_seen_ = 0;   // the machine's NMI edge count, as far as taken
    bool nmi_pending_ = false;
    std::uint64_t stop_frames_ = 0;
    std::uint64_t frames_seen_ = 0;
};

} // namespace qilin::game
