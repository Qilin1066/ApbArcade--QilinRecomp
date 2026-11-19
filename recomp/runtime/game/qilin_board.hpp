#pragma once
// The main CPU's board for the game: lifted T-11 code (t11_rt.hpp) on the
// System 2 machine (system2_machine.hpp), with the scheduler in game.hpp
// running the sound CPU, the board's events and the interrupts at every
// service point.

#include "system2_machine.hpp"
#include "t11_rt.hpp"

namespace qilin::game {
class Scheduler;
}

namespace qilin::rt {

struct Board {
    Cpu cpu;
    std::int32_t icount = 0;
    std::uint8_t* bank = nullptr;   // the machine's bank registers
    sys2::Machine* machine = nullptr;
    game::Scheduler* scheduler = nullptr;

    // RAM and ROM don't care when they're read. The rest of the map does (the
    // latches, the sound reset, the scroll registers and video RAM, by the
    // scanline): there the machine's clock is brought to the cycle MAME's
    // devices would see, the slice's start plus the cycles the lifted code has
    // charged, the current instruction's included (icount, stored before it).
    static constexpr bool timed(std::uint16_t a) noexcept { return a >= 0x1000 && a < 0x4000; }
    struct Now {
        Board& b;
        std::int64_t was;
        explicit Now(Board& board);
        ~Now() { b.machine->main_time = was; }
    };
    void fetch(std::uint16_t a) { machine->main_fetch(a); }
    std::uint16_t read16(std::uint16_t a) {
        if (!timed(a)) return machine->main_read16(a);
        Now now(*this);
        return machine->main_read16(a);
    }
    void write16(std::uint16_t a, std::uint16_t v) {
        if (!timed(a)) return machine->main_write16(a, v);
        Now now(*this);
        machine->main_write16(a, v);
    }
    std::uint8_t read8(std::uint16_t a) {
        if (!timed(a)) return machine->main_read8(a);
        Now now(*this);
        return machine->main_read8(a);
    }
    void write8(std::uint16_t a, std::uint8_t v) {
        if (!timed(a)) return machine->main_write8(a, v);
        Now now(*this);
        machine->main_write8(a, v);
    }
    void service();
    void wait();
    bool interrupt_due();
    void check_interrupts();   // after RTI, RTT or MTPS: take one now, as MAME does in the same step
    void reset_output() {}   // nothing on System 2 listens to the T-11's RESET output
    void unlifted();
};

} // namespace qilin::rt
