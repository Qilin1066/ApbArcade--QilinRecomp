#pragma once
// The sound CPU's board for the game: lifted 6502 code (m6502_rt.hpp) on the
// System 2 machine. The scheduler (game.hpp) runs it in slices behind the
// main CPU and ends each slice by setting 'yielding'.

#include "m6502_rt.hpp"
#include "system2_machine.hpp"

namespace qilin::game {
class Scheduler;
}

namespace qilin::rt6502 {

struct Board {
    Cpu cpu;
    std::int32_t icount = 0;
    bool yielding = false;
    sys2::Machine* machine = nullptr;
    game::Scheduler* scheduler = nullptr;

    // The chips (0x1800-0x1FFF and its mirror) see the cycle MAME's would:
    // the slice's start plus the bus cycles before this one (icount, stored
    // by the lifted code before each access). RAM, EEPROM and ROM don't care.
    static constexpr bool timed(std::uint16_t a) noexcept { return a < 0x4000 && (a & 0x1fffu) >= 0x1800; }
    struct Now {
        Board& b;
        std::int64_t was;
        explicit Now(Board& board);
        ~Now() { b.machine->sound_time = was; }
    };
    std::uint8_t read(std::uint16_t a) {
        if (!timed(a)) return machine->sound_read(a);
        Now now(*this);
        return machine->sound_read(a);
    }
    std::uint8_t read_arg(std::uint16_t a) { return read(a); }
    std::uint8_t read_sync(std::uint16_t a) { return read(a); }
    void write(std::uint16_t a, std::uint8_t v) {
        if (!timed(a)) return machine->sound_write(a, v);
        Now now(*this);
        machine->sound_write(a, v);
    }
    void service();
    // Around the instructions that change the I flag (MAME decides on
    // interrupts at each instruction's end): -1 before CLI, PLP or SEI, 1
    // after CLI or PLP, 0 after RTI. The board decides at its next service.
    void interrupt_window(int when);
    void unlifted();
};

} // namespace qilin::rt6502
