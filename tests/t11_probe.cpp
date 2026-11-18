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

constexpr bool test_interrupt_latches() {
    using qilin::system2::IrqSource;
    qilin::system2::Interrupts interrupts;
    interrupts.sample(IrqSource::scanline);
    if (interrupts.control_pins() != 0) {
        return false;
    }
    interrupts.set_enable(0xff);
    interrupts.sample(IrqSource::scanline);
    interrupts.sample(IrqSource::sound_reply_written);
    interrupts.set_enable(0);
    if (interrupts.control_pins() != 6) {
        return false;
    }
    interrupts.sample(IrqSource::scanline);
    if (interrupts.control_pins() != 2) {
        return false;
    }
    interrupts.acknowledge(IrqSource::sound_reply_written);
    return interrupts.control_pins() == 0;
}

static_assert(test_interrupt_latches());

bool test_interrupt_entry() {
    using qilin::system2::IrqSource;
    qilin::t11::System2State board;
    board.interrupts.set_enable(0x0c);
    board.interrupts.sample(IrqSource::scanline);
    board.interrupts.sample(IrqSource::vblank);
    board.ram[0x36] = 0x6032;
    board.ram[0x37] = 0xa4;
    board.cpu.flags = {.n = true, .c = true};
    board.cpu.r[6] = 0x400;
    if (qilin::t11::accept_system2_irq(board, 0x8200, {}).has_value()) {
        return false;
    }
    board.psw_control = 0xc0;
    auto bad_stack = board;
    bad_stack.cpu.r[6] = 2;
    const auto error = qilin::t11::run_system2(bad_stack, 1);
    if (error.reason != StopReason::interrupt_stack || error.address != 0x8200 || error.vector != 0x6c ||
        bad_stack.cpu.r[6] != 2 || bad_stack.ram != board.ram) {
        return false;
    }
    if (qilin::t11::run_system2(board, 0).reason != StopReason::budget || board.cpu.r[6] != 0x400) {
        return false;
    }
    const auto irq = qilin::t11::run_system2(board, 1);
    const auto& flags = board.cpu.flags;
    return irq.reason == StopReason::interrupt && irq.address == 0x6032 && irq.vector == 0x6c &&
           board.cpu.r[6] == 0x3fc && board.ram[0x1fe] == 0x8200 && board.ram[0x1ff] == 0xc9 &&
           board.psw_control == 0xa0 && !flags.n && flags.z && !flags.v && !flags.c &&
           board.interrupts.control_pins() == 0x0c && board.ram[0x100] == 0;
}

constexpr bool test_video_selection() {
    qilin::system2::VideoMemory video;
    video.write_window(0x2000, 0x3333);
    video.observe(0x8080);
    if (video.bank() != 3) {
        return false;
    }
    video.observe(0x8001);
    video.observe(0x8080);
    video.write_window(0x2000, 0x1111);
    video.observe(0x8000);
    video.observe(0x80a0);
    video.write_window(0x2000, 0x9999);
    if (video.bank() != 1 || video.read_window(0x2000) != 0xffff) {
        return false;
    }
    video.observe(0x8000);
    video.observe(0x005a);
    video.observe(0x0100); // Break alternate validation.
    video.observe(0x80c0);
    if (video.bank() != 2 || video.read_window(0x2000) != 0) {
        return false;
    }
    video.observe(0x8000);
    video.observe(0x005a);
    video.observe(0xfa28);
    video.observe(0xfa48);
    video.observe(0x80e0); // Commit the staged alternate selection.
    if (video.bank() != 0 || video.read_window(0x2000) != 0x1111) {
        return false;
    }
    video.observe(0x8000);
    video.observe(0xe980);
    video.observe(0x8080);
    video.observe(0xe982); // Odd phase sets bit zero.
    video.observe(0xe980); // Even phase sets bit one.
    video.observe(0xe9a0);
    return video.bank() == 3 && video.read_window(0x2000) == 0x3333;
}

static_assert(test_video_selection());

constexpr bool test_palette_ram() {
    using qilin::system2::Rgb8;
    qilin::t11::System2State board;
    board.write_data_word(0x1000, 0xfff0);
    board.write_data_word(0x1002, 0xf0f1);
    board.write_data_word(0x1286, 0x4b2d);
    board.write_data_word(0x13fe, 0xffff);
    return board.read_data_word(0x1200) == 0xfff0 &&
           board.read_data_word(0x1086) == 0x4b2d &&
           board.read_data_word(0x11fe) == 0xffff &&
           board.palette.color(0) == Rgb8{0, 0, 0} &&
           board.palette.color(1) == Rgb8{116, 0, 116} &&
           board.palette.color(0x43) == Rgb8{89, 194, 59} &&
           board.palette.color(0xff) == Rgb8{240, 240, 240};
}

static_assert(test_palette_ram());

bool test_indirect_words() {
    qilin::t11::System2State board;
    board.cpu.r[5] = 0x0301;
    board.ram[2] = 0xabcd;
    board.ram[0x181] = 11;
    board.ram[0x183] = 5;
    board.ram[0x184] = 7;
    board.ram[0x185] = 9;
    board.ram[0x186] = 10;
    const auto stop = qilin::t11::run_system2(board, 64);
    const auto& flags = board.cpu.flags;
    if (stop.reason != StopReason::halt || stop.address != 0x609a || board.cpu.r[5] != 0x030d ||
        board.ram[0x180] != 0x0301 || board.ram[0x181] != 11 || board.ram[0x182] != 11 ||
        board.ram[0x183] != 4 || board.ram[0x184] != 11 || board.ram[0x186] != 10 ||
        !flags.n || flags.z || flags.v || !flags.c || board.cpu.r[4] != 0x11ad ||
        board.cpu.r[3] != 0xfff4 || board.ram[2] != 0 || board.ram[0x187] != 0x11ad ||
        board.video.bank() != 0 || board.video.read_window(0x2100) != 0xabcd ||
        board.cpu.r[0] != 0x4b2d || board.palette.read_word(0x1086) != 0x002d ||
        board.palette.color(0x43) != qilin::system2::Rgb8{0, 0, 59}) {
        return false;
    }
    board.video.observe(0x8000);
    board.video.observe(0x80c0);
    if (board.video.bank() != 2 || board.video.read_window(0x2100) != 0x0034) {
        return false;
    }

    qilin::t11::System2State bad_write;
    bad_write.cpu.r[5] = 0x3ffd;
    bad_write.video.write_window(0x3ffe, 0x8080);
    const auto write_stop = qilin::t11::run_system2(bad_write, 64);
    if (write_stop.reason != StopReason::memory_write || write_stop.address != 0x6034 ||
        write_stop.memory_address != 0x4000 || write_stop.banks != qilin::t11::Banks{1, 35} ||
        bad_write.cpu.r[5] != 0x3fff || bad_write.video.read_window(0x3ffc) != 0x3ffd ||
        bad_write.video.read_window(0x3ffe) != 0x8080 || bad_write.cpu.flags.n) {
        return false;
    }

    qilin::t11::System2State bad_read;
    bad_read.cpu.r[5] = 0x3fff;
    const auto read_stop = qilin::t11::run_system2(bad_read, 64);
    return read_stop.reason == StopReason::memory_read && read_stop.address == 0x6034 &&
           read_stop.memory_address == 0x4000 && read_stop.banks == qilin::t11::Banks{1, 35} &&
           bad_read.cpu.r[5] == 0x4001 && bad_read.video.read_window(0x3ffe) == 0x3fff;
}

int test_system2() {
    using qilin::t11::Banks;
    using qilin::t11::System2State;
    using qilin::system2::IrqSource;

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
        complete.interrupts.set_enable(0x0f);
        for (const auto source : {IrqSource::sound_command_read, IrqSource::sound_reply_written,
                                  IrqSource::scanline, IrqSource::vblank}) {
            complete.interrupts.sample(source);
        }
        const auto boundary = qilin::t11::run_system2(complete, mode == 0 ? 27 : 30);
        const auto& result = complete.cpu.flags;
        const bool moved_negative_word = mode == 1;
        if (boundary.reason != StopReason::budget || boundary.address != 0x6032 || boundary.banks != Banks{1, 35} ||
            complete.cpu.r[0] != 0x1111 || complete.cpu.r[1] != 0x2222 || complete.cpu.r[2] != 0x3333 ||
            complete.watchdog.ticks_left() != 1'048'576 ||
            complete.interrupts.enabled() != 0x0d || complete.interrupts.control_pins() != 2 ||
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
    if (!test_interrupt_entry()) {
        std::cerr << "Unexpected native interrupt entry or saved T-11 stack frame.\n";
        return 1;
    }
    if (!test_indirect_words()) {
        std::cerr << "Unexpected result from native register-indirect memory access.\n";
        return 1;
    }
    std::cout << "Native System 2 code passed bank, watchdog, IRQ, video-memory, and palette checks.\n";
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
