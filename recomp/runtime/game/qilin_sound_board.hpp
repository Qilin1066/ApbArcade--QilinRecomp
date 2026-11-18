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

    std::uint8_t read(std::uint16_t a) { return machine->sound_read(a); }
    std::uint8_t read_arg(std::uint16_t a) { return machine->sound_read(a); }
    std::uint8_t read_sync(std::uint16_t a) { return machine->sound_read(a); }
    void write(std::uint16_t a, std::uint8_t v) { machine->sound_write(a, v); }
    void service();
    void unlifted();
};

} // namespace qilin::rt6502
