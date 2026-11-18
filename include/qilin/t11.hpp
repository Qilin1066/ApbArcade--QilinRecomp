#pragma once

#include <qilin/system2.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace qilin::t11 {

struct Flags {
    bool n{};
    bool z{};
    bool v{};
    bool c{};
};

struct State {
    std::array<std::uint16_t, 7> r{};
    Flags flags{};
};

using Banks = std::array<std::uint8_t, 2>;
inline constexpr unsigned system2_ram_size = 0x1000;

struct System2State {
    State cpu{};
    std::array<std::uint16_t, system2_ram_size / 2> ram{};
    system2::Watchdog watchdog{};
    system2::Interrupts interrupts{};
    std::uint8_t psw_control = 0xe0;
};

enum class StopReason { halt, budget, watchdog, interrupt, interrupt_stack };

struct Stop {
    StopReason reason;
    std::uint16_t address;
    Banks banks{};
    std::uint16_t vector{};
};

// System 2 uses the T-11's internally selected vectors. CP0–CP3 form one code.
[[nodiscard]] constexpr std::optional<Stop> accept_system2_irq(
    System2State& board, std::uint16_t pc, Banks banks) noexcept {
    const auto pins = board.interrupts.control_pins();
    if (pins == 0 || (4 + pins / 4) <= (board.psw_control >> 5)) {
        return std::nullopt;
    }
    constexpr std::array<std::uint16_t, 16> vectors{
        0, 0x38, 0x34, 0x30, 0x5c, 0x58, 0x54, 0x50,
        0x4c, 0x48, 0x44, 0x40, 0x6c, 0x68, 0x64, 0x60
    };
    const auto vector = vectors[pins];
    const auto sp = board.cpu.r[6];
    if (sp < 4 || sp > system2_ram_size || (sp & 1) != 0) {
        return Stop{StopReason::interrupt_stack, pc, banks, vector};
    }

    const auto handler = board.ram[vector / 2];
    const auto next_psw = board.ram[vector / 2 + 1];
    auto& flags = board.cpu.flags;
    const auto old_psw = static_cast<std::uint16_t>((board.psw_control & 0xf0) |
        (flags.n ? 8 : 0) | (flags.z ? 4 : 0) | (flags.v ? 2 : 0) | (flags.c ? 1 : 0));
    board.ram[(sp - 2) / 2] = old_psw;
    board.ram[(sp - 4) / 2] = pc;
    board.cpu.r[6] = static_cast<std::uint16_t>(sp - 4);
    board.psw_control = static_cast<std::uint8_t>(next_psw & 0xf0);
    flags = {(next_psw & 8) != 0, (next_psw & 4) != 0, (next_psw & 2) != 0, (next_psw & 1) != 0};
    return Stop{StopReason::interrupt, handler, banks, vector};
}

// Defined by the generated translation unit. Execution starts at its entry address.
[[nodiscard]] Stop run(State& state, std::uint64_t budget);
[[nodiscard]] Stop run_system2(System2State& board, std::uint64_t budget);

namespace ops {

constexpr std::uint16_t sign = 0x8000;

constexpr void set_nz(Flags& flags, std::uint16_t value) {
    flags.n = (value & sign) != 0;
    flags.z = value == 0;
}

constexpr std::uint16_t move(Flags& flags, std::uint16_t source) {
    set_nz(flags, source);
    flags.v = false;
    return source;
}

constexpr std::uint16_t add(Flags& flags, std::uint16_t left, std::uint16_t right) {
    const auto sum = static_cast<std::uint32_t>(left) + right;
    const auto result = static_cast<std::uint16_t>(sum);
    set_nz(flags, result);
    flags.v = (~(left ^ right) & (left ^ result) & sign) != 0;
    flags.c = sum > 0xffff;
    return result;
}

constexpr std::uint16_t subtract(Flags& flags, std::uint16_t left, std::uint16_t right) {
    const auto result = static_cast<std::uint16_t>(left - right);
    set_nz(flags, result);
    flags.v = ((left ^ right) & (left ^ result) & sign) != 0;
    flags.c = left < right;
    return result;
}

constexpr std::uint16_t clear(Flags& flags) {
    flags = {.z = true};
    return 0;
}

constexpr std::uint16_t clear_byte(Flags& flags, std::uint16_t word, bool high_byte) {
    flags = {.z = true};
    const std::uint16_t mask = high_byte ? 0x00ff : 0xff00;
    return static_cast<std::uint16_t>(word & mask);
}

constexpr std::uint16_t increment(Flags& flags, std::uint16_t value) {
    const auto result = static_cast<std::uint16_t>(value + 1);
    set_nz(flags, result);
    flags.v = result == sign;
    return result;
}

constexpr std::uint16_t decrement(Flags& flags, std::uint16_t value) {
    const auto result = static_cast<std::uint16_t>(value - 1);
    set_nz(flags, result);
    flags.v = value == sign;
    return result;
}

constexpr void test(Flags& flags, std::uint16_t value) {
    set_nz(flags, value);
    flags.v = false;
    flags.c = false;
}

} // namespace ops
} // namespace qilin::t11
