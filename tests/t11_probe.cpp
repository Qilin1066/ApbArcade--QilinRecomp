#include <qilin/t11.hpp>

#include <iostream>
#include <string_view>

namespace {

using qilin::t11::Flags;
using qilin::t11::StopReason;
namespace ops = qilin::t11::ops;

constexpr bool test_subtraction() {
    Flags flags{};
    const auto result = ops::subtract(flags, 0, 1);
    if (result != 0xffff || !flags.n || flags.z || flags.v || !flags.c) {
        return false;
    }
    return ops::subtract(flags, 0x8000, 1) == 0x7fff &&
           !flags.n && !flags.z && flags.v && !flags.c;
}

constexpr bool test_decrement_and_test() {
    Flags flags{.c = true};
    if (ops::decrement(flags, 0x8000) != 0x7fff || !flags.v || !flags.c) {
        return false;
    }
    ops::test(flags, 0);
    return !flags.n && flags.z && !flags.v && !flags.c;
}

static_assert(test_subtraction());
static_assert(test_decrement_and_test());

constexpr bool test_watchdog_timing() {
    qilin::system2::Watchdog watchdog;
    watchdog.advance(1'048'575);
    if (watchdog.reset_pending() || watchdog.ticks_left() != 1) {
        return false;
    }
    watchdog.strobe();
    if (watchdog.ticks_left() != 1'048'576) {
        return false;
    }
    watchdog.advance(1'048'576);
    watchdog.strobe();
    if (!watchdog.reset_pending()) {
        return false;
    }
    watchdog.reset();
    watchdog.advance(0x1'0000'0000ULL);
    return watchdog.reset_pending();
}

static_assert(test_watchdog_timing());

int test_system2() {
    using qilin::t11::Banks;
    using qilin::t11::System2State;

    System2State before_jump;
    before_jump.ram[0x103] = 0xa55a;
    const auto cmp = qilin::t11::run_system2(before_jump, 10);
    const auto& flags = before_jump.cpu.flags;
    if (cmp.reason != StopReason::budget || cmp.address != 0x8232 || cmp.banks != Banks{0, 0} ||
        before_jump.ram[0x100] != 0x8100 || before_jump.ram[0x101] != 0x0023 ||
        before_jump.ram[0x102] != 0x8123 || before_jump.ram[0x103] != 0 || before_jump.cpu.r[3] != 0x8123 ||
        !flags.n || flags.z || flags.v || !flags.c) {
        std::cerr << "Unexpected result after absolute RAM and ROM operands.\n";
        return 1;
    }

    System2State first_switch;
    const auto new_bank = qilin::t11::run_system2(first_switch, 13);
    if (new_bank.reason != StopReason::budget || new_bank.address != 0x400a || new_bank.banks != Banks{1, 0} ||
        first_switch.cpu.r[0] != 0x1111 || first_switch.cpu.r[1] != 0) {
        std::cerr << "Window 0 did not switch at the expected instruction.\n";
        return 1;
    }

    System2State cross_window;
    const auto second_bank = qilin::t11::run_system2(cross_window, 19);
    if (second_bank.reason != StopReason::budget || second_bank.address != 0x6002 ||
        second_bank.banks != Banks{1, 35} || cross_window.cpu.r[1] != 0x2222 ||
        cross_window.cpu.r[2] != 0 || !cross_window.cpu.flags.n) {
        std::cerr << "Window 1 did not switch after fetching the operand from the old bank.\n";
        return 1;
    }

    for (std::uint16_t mode = 0; mode < 3; ++mode) {
        System2State complete;
        complete.cpu.r[4] = mode;
        complete.watchdog.advance(1'048'575);
        const auto halt = qilin::t11::run_system2(complete, 64);
        const auto& result = complete.cpu.flags;
        const bool moved_negative_word = mode == 1;
        if (halt.reason != StopReason::halt || halt.address != 0x6020 || halt.banks != Banks{1, 35} ||
            complete.cpu.r[0] != 0x1111 || complete.cpu.r[1] != 0x2222 || complete.cpu.r[2] != 0x3333 ||
            complete.watchdog.ticks_left() != 1'048'576 ||
            result.n != moved_negative_word || result.z == moved_negative_word || result.v || result.c) {
            std::cerr << "Unexpected result from the native watchdog write in mode " << mode << ".\n";
            return 1;
        }
    }

    System2State expired;
    expired.watchdog.advance(1'048'576);
    const auto watchdog = qilin::t11::run_system2(expired, 64);
    if (watchdog.reason != StopReason::watchdog || watchdog.address != 0x8200 ||
        watchdog.banks != Banks{0, 0} || expired.ram[0x100] != 0 || !expired.watchdog.reset_pending()) {
        std::cerr << "Native execution did not stop for the pending board reset.\n";
        return 1;
    }
    std::cout << "Native System 2 program switched both ROM windows and serviced the watchdog.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--system2") {
        return test_system2();
    }
    qilin::t11::State complete;
    const auto halt = qilin::t11::run(complete, 100);
    if (halt.reason != StopReason::halt || halt.address != 0x022a ||
        complete.r[0] != 0 || complete.r[1] != 6 || complete.r[2] != 0 ||
        complete.r[3] != 0x8000 || complete.r[4] != 0 ||
        !complete.flags.n || complete.flags.z || !complete.flags.v || !complete.flags.c) {
        std::cerr << "Unexpected synthetic T-11 result.\n";
        return 1;
    }

    qilin::t11::State limited;
    const auto budget = qilin::t11::run(limited, 5);
    if (budget.reason != StopReason::budget || budget.address != 0x0208 ||
        limited.r[0] != 2 || limited.r[1] != 5) {
        std::cerr << "Unexpected instruction-budget result.\n";
        return 1;
    }

    std::cout << "Synthetic T-11 program ran as precompiled host code.\n";
}
