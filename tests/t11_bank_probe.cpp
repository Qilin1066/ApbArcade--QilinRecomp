#include <qilin/t11.hpp>

#include <cstdint>
#include <iostream>

int main() {
    using namespace qilin::t11;
    for (unsigned selector = 0; selector < 64; ++selector) {
        const auto bank = static_cast<std::uint8_t>(16 * (selector / 16) +
            4 * (3 - selector % 4) + (selector / 4) % 4);
        const auto word = static_cast<std::uint16_t>(selector * 0x400 + (selector == 0 ? 0 : 0x1ff));
        System2State boundary;
        boundary.cpu.r[0] = word;
        boundary.cpu.r[2] = 0x7fff;
        boundary.cpu.flags.c = true;
        const auto expected_banks = Banks{1, bank};
        const auto stop = run_system2(boundary, 5);
        const auto& flags = boundary.cpu.flags;
        if (stop.reason != StopReason::budget || stop.address != 0x6002 || stop.banks != expected_banks ||
            boundary.cpu.r[0] != word || flags.n != (selector >= 32) || flags.z != (selector == 0) || flags.v || !flags.c) {
            std::cerr << "Unexpected state after selector " << selector << '\n';
            return 1;
        }

        System2State complete;
        complete.cpu.r[0] = word;
        complete.cpu.r[3] = 0x6004;
        complete.cpu.r[4] = 0xab01;
        complete.cpu.r[6] = 0x202;
        complete.ram[0x100] = 0xffff;
        complete.sound.coin_outputs = complete.sound.switch_outputs = complete.sound.chip_reset = 1;
        complete.interrupts.set_enable(3);
        complete.sound.write_reply(0xa5, complete.interrupts);
        const auto halt = run_system2(complete, 21);
        if (halt.reason != StopReason::halt || halt.address != 0x6026 || halt.banks != expected_banks ||
            complete.ram[0xff] != 0x6026 || complete.ram[0xfe] != 0xab11 ||
            complete.cpu.r[1] != 0x1000u + bank || complete.ram[0x100] != bank ||
            complete.cpu.r[0] != complete.cpu.r[1] || complete.cpu.r[2] != 0xffa5 ||
            complete.sound.cpu_reset != ((bank & 1u) != 0) || complete.sound.chip_reset ||
            complete.sound.coin_outputs || complete.sound.switch_outputs || complete.sound.reply_pending ||
            !complete.sound.command_pending || complete.interrupts.control_pins() != 0 ||
            complete.sound.read_command(complete.interrupts) != 0x80 || complete.sound.command_pending ||
            complete.interrupts.control_pins() != 1 ||
            complete.cpu.r[3] != 0x6006 || complete.cpu.r[4] != 0xab11 || complete.cpu.r[5] != 0xff80 ||
            complete.cpu.r[6] != 0x200 || !complete.cpu.flags.n || complete.cpu.flags.z || complete.cpu.flags.v) {
            std::cerr << "Wrong precompiled bank for selector " << selector << '\n';
            return 1;
        }
    }

    for (const auto source : {0x7001u, 0x6004u}) {
        System2State board;
        board.cpu.r[3] = static_cast<std::uint16_t>(source);
        board.cpu.r[6] = 0x4002;
        const auto stop = run_system2(board, 8);
        const bool missing = source == 0x7001;
        if (stop.reason != (missing ? StopReason::memory_read : StopReason::memory_write) ||
            stop.address != 0x6006 || stop.memory_address != (missing ? source : 0x4000u) ||
            board.cpu.r[3] != source || board.cpu.r[6] != 0x4002) {
            std::cerr << "Incorrect byte bus diagnostic\n";
            return 1;
        }
    }
}
