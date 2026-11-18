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
        const auto halt = run_system2(complete, 7);
        if (halt.reason != StopReason::halt || halt.address != 0x6006 || halt.banks != expected_banks ||
            complete.cpu.r[1] != 0x1000u + bank) {
            std::cerr << "Wrong precompiled bank for selector " << selector << '\n';
            return 1;
        }
    }
}
