#pragma once
// The differential test's board: memory, ROM banking, and a trace of every
// bus access. The lifted code and MAME's T-11 core each run on one, from
// the same starting state, and their traces must match exactly.
//
// 0x0000-0x3FFF is plain memory here (devices aren't modelled: the test is
// of the CPU), except that word writes to 0x1400-0x147F select ROM banks as
// on System 2. ROM is MAME's maincpu region, banked as the board banks it.

#include "system2.hpp"
#include "t11_rt.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace qilin::rt {

enum class Access : std::uint8_t { fetch, read16, write16, read8, write8, reset };

struct Event {
    Access access;
    std::uint16_t address;
    std::uint16_t value;
    bool operator==(const Event&) const = default;
};

// Ends a trial early.
struct Stop {
    enum Reason { budget, wait, unlifted, runaway } reason;
};

struct Memory {
    std::array<std::uint8_t, 0x4000> ram{};
    std::span<const std::uint8_t> rom;     // maincpu region, 0x90000 bytes
    std::uint8_t bank[2] = {0, 0};
    std::vector<Event> trace;

    std::uint8_t byte_at(std::uint16_t a) const {
        if (a < 0x4000) return ram[a];
        return rom[sys2::rom_offset(a, bank[0], bank[1])];
    }
    std::uint16_t word_at(std::uint16_t a) const {
        a = static_cast<std::uint16_t>(a & 0xfffe);
        return static_cast<std::uint16_t>(byte_at(a) | (byte_at(static_cast<std::uint16_t>(a + 1)) << 8));
    }
    void select(std::uint16_t a, std::uint16_t word) {
        if (sys2::area(a) == sys2::Area::adc) bank[(a >> 1) & 1] = sys2::select_rom_bank(word);
    }

    std::uint16_t fetch_word(std::uint16_t a) {
        const auto v = word_at(a);
        trace.push_back({Access::fetch, a, v});
        return v;
    }
    std::uint16_t read_word(std::uint16_t a) {
        const auto v = word_at(a);
        trace.push_back({Access::read16, a, v});
        return v;
    }
    void write_word(std::uint16_t a, std::uint16_t v) {
        trace.push_back({Access::write16, a, v});
        if (a < 0x4000) {
            ram[a & 0xfffe] = static_cast<std::uint8_t>(v);
            ram[(a & 0xfffe) + 1] = static_cast<std::uint8_t>(v >> 8);
        }
        select(a, v);
    }
    std::uint8_t read_byte(std::uint16_t a) {
        const auto v = byte_at(a);
        trace.push_back({Access::read8, a, v});
        return v;
    }
    void write_byte(std::uint16_t a, std::uint8_t v) {
        trace.push_back({Access::write8, a, v});
        if (a < 0x4000) ram[a] = v;
        // A byte reaches a 16-bit handler in its own lane.
        select(static_cast<std::uint16_t>(a & 0xfffe), static_cast<std::uint16_t>((a & 1) ? v << 8 : v));
    }
};

struct Board : Memory {
    Cpu cpu;
    std::int32_t icount = 0;

    void fetch(std::uint16_t a) { fetch_word(a); }
    std::uint16_t read16(std::uint16_t a) { return read_word(a); }
    void write16(std::uint16_t a, std::uint16_t v) { write_word(a, v); }
    std::uint8_t read8(std::uint16_t a) { return read_byte(a); }
    void write8(std::uint16_t a, std::uint8_t v) { write_byte(a, v); }
    void service() {
        if (icount <= 0) throw Stop{Stop::budget};
    }
    void wait() { throw Stop{Stop::wait}; }
    void reset_output() { trace.push_back({Access::reset, 0, 0}); }
    [[noreturn]] void unlifted() { throw Stop{Stop::unlifted}; }
};

} // namespace qilin::rt
