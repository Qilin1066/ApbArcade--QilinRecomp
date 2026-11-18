#pragma once
// One differential trial: a state run in MAME's T-11 core and in the lifted
// code, compared on registers, PSW, cycles and the whole bus trace in order.
// Shared by qilin_difftest (the game's functions) and qilin_lifter_check
// (every instruction form, in CI).

#include "qilin_board.hpp"

#include "t11_reference.h"

#include <format>
#include <string>

namespace qilin::difftest {

using rt::Access;
using rt::Event;

constexpr std::uint16_t sentinel = 0x0002;   // the return address the test pushes: never code
constexpr int step_limit = 200'000;

struct ReferenceBus final : t11_device::Bus {
    rt::Memory& m;
    explicit ReferenceBus(rt::Memory& memory) : m(memory) {}
    std::uint16_t fetch(std::uint16_t a) override { return m.fetch_word(a); }
    std::uint16_t read_word(std::uint16_t a) override { return m.read_word(a); }
    void write_word(std::uint16_t a, std::uint16_t d) override { m.write_word(a, d); }
    std::uint8_t read_byte(std::uint16_t a) override { return m.read_byte(a); }
    void write_byte(std::uint16_t a, std::uint8_t d) override { m.write_byte(a, d); }
    void reset_output() override { m.trace.push_back({Access::reset, 0, 0}); }
};

struct State {
    rt::Memory memory;
    rt::Cpu cpu;
};

enum class Outcome { match, mismatch, skip_runaway, skip_wait, skip_unlifted, skip_left };

inline const char* describe(Access a) {
    switch (a) {
    case Access::fetch: return "fetch";
    case Access::read16: return "read16";
    case Access::write16: return "write16";
    case Access::read8: return "read8";
    case Access::write8: return "write8";
    case Access::reset: return "reset";
    }
    return "?";
}

inline std::string event_text(const Event& e) {
    return std::format("{} {:04X}={:04X}", describe(e.access), e.address, e.value);
}

struct Result {
    Outcome outcome;
    std::string detail;
};

inline Result trial(const State& start, const rt::Entry& entry) {
    // MAME's core.
    rt::Memory ref_memory = start.memory;
    ReferenceBus bus(ref_memory);
    t11_device cpu(bus, 0x36ff);
    cpu.reset();
    for (int r = 0; r < 8; ++r) cpu.set_reg(r, start.cpu.r[r]);
    cpu.set_psw(start.cpu.psw);
    long cycles = 0;
    int steps = 0;
    while (cpu.reg(7) != sentinel) {
        if (++steps > step_limit) return {Outcome::skip_runaway, {}};
        if (cpu.reg(7) < 0x4000) return {Outcome::skip_left, {}};   // into RAM: not the program
        cycles += cpu.step();
        if (cpu.waiting()) return {Outcome::skip_wait, {}};
    }

    // The lifted code.
    rt::Board b;
    static_cast<rt::Memory&>(b) = start.memory;
    b.cpu = start.cpu;
    const std::int32_t budget = static_cast<std::int32_t>(cycles) + 1000;
    b.icount = budget;
    try {
        entry.fn(b, 0);
        int hops = 0;
        while (b.cpu.r[7] != sentinel) {
            if (++hops > step_limit || b.cpu.r[7] < 0x4000) return {Outcome::mismatch, "the lifted code didn't return"};
            if (!rt::dispatch(b)) b.unlifted();
        }
    } catch (const rt::Stop& stop) {
        if (stop.reason == rt::Stop::unlifted) return {Outcome::skip_unlifted, {}};
        if (stop.reason == rt::Stop::wait) return {Outcome::skip_wait, {}};
        return {Outcome::mismatch, "the lifted code ran past the original's cycle count"};
    }

    // Compare.
    std::string diff;
    const auto& a = ref_memory.trace;
    const auto& l = b.trace;
    for (std::size_t i = 0; i < std::max(a.size(), l.size()); ++i) {
        if (i >= a.size() || i >= l.size() || !(a[i] == l[i])) {
            diff += std::format("bus access {}: MAME {}, lifted {}\n", i, i < a.size() ? event_text(a[i]) : "(end)",
                                i < l.size() ? event_text(l[i]) : "(end)");
            if (i > 0) diff += std::format("  after {}\n", event_text(a[i - 1]));
            break;
        }
    }
    for (int r = 0; r < 7; ++r) {
        if (cpu.reg(r) != b.cpu.r[r]) diff += std::format("R{}: MAME {:04X}, lifted {:04X}\n", r, cpu.reg(r), b.cpu.r[r]);
    }
    if (cpu.psw() != b.cpu.psw) diff += std::format("PSW: MAME {:03o}, lifted {:03o}\n", cpu.psw(), b.cpu.psw);
    if (budget - b.icount != cycles) diff += std::format("cycles: MAME {}, lifted {}\n", cycles, budget - b.icount);
    if (ref_memory.bank[0] != b.bank[0] || ref_memory.bank[1] != b.bank[1]) diff += "banks differ\n";
    if (!diff.empty()) return {Outcome::mismatch, diff};
    return {Outcome::match, {}};
}


} // namespace qilin::difftest
