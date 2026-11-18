#pragma once

#include <qilin/system2.hpp>

#include <array>
#include <cstdint>

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
};

enum class StopReason { halt, budget, watchdog };

struct Stop {
    StopReason reason;
    std::uint16_t address;
    Banks banks{};
};

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
