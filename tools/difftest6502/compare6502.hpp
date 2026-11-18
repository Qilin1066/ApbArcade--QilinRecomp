#pragma once
// One 6502 differential trial: a state run in MAME's 6502 core and in the
// lifted code, compared on A, X, Y, P, S, cycles and the whole bus trace in
// order (opcode fetches, operand fetches, dummy reads, writes). Shared by
// qilin_sound_difftest (the game's functions) and qilin_sound_lifter_check
// (every instruction form, in CI).

#include "qilin_sound_board.hpp"

#include "m6502_decoder.hpp"
#include "m6502_reference.h"

#include <format>
#include <string>

namespace qilin::difftest6502 {

using rt6502::Access;
using rt6502::Event;

// The address the test returns to: RAM, never code.
constexpr std::uint16_t sentinel = 0x3ff0;
constexpr int step_limit = 200'000;

struct ReferenceBus final : m6502_device::Bus {
    rt6502::Memory& m;
    explicit ReferenceBus(rt6502::Memory& memory) : m(memory) {}
    std::uint8_t read(std::uint16_t a) override { return m.read(a); }
    std::uint8_t read_sync(std::uint16_t a) override { return m.read_sync(a); }
    std::uint8_t read_arg(std::uint16_t a) override { return m.read_arg(a); }
    void write(std::uint16_t a, std::uint8_t v) override { m.write(a, v); }
};

struct State {
    rt6502::Memory memory;
    rt6502::Cpu cpu;   // cpu.pc: the function's entry
};

// Pushes the return the trial ends on: a JSR's return address, or an
// interrupt's frame (for a handler, which ends with RTI).
inline void push_return(State& s, bool interrupt) {
    const auto push = [&](std::uint8_t v) {
        s.memory.bytes[0x100u | (s.cpu.sp & 0xff)] = v;
        s.cpu.sp = static_cast<std::uint16_t>(0x100 | ((s.cpu.sp - 1) & 0xff));
    };
    const std::uint16_t back = interrupt ? sentinel : static_cast<std::uint16_t>(sentinel - 1);
    push(static_cast<std::uint8_t>(back >> 8));
    push(static_cast<std::uint8_t>(back));
    if (interrupt) push(static_cast<std::uint8_t>(s.cpu.p & ~rt6502::F_B));
}

// skip_left: the original left the program, into RAM or into data (a KIL).
enum class Outcome { match, mismatch, skip_runaway, skip_unlifted, skip_left };

inline const char* describe(Access a) {
    switch (a) {
    case Access::read: return "read";
    case Access::arg: return "arg";
    case Access::sync: return "sync";
    case Access::write: return "write";
    }
    return "?";
}

inline std::string event_text(const Event& e) { return std::format("{} {:04X}={:02X}", describe(e.access), e.address, e.value); }

struct Result {
    Outcome outcome;
    std::string detail;
    int yields = 0;
};

inline Result trial(const State& start, rt6502::LiftedFn fn, std::int32_t slice) {
    // MAME's core.
    rt6502::Memory ref_memory = start.memory;
    ReferenceBus bus(ref_memory);
    m6502_device cpu(bus);
    cpu.set_state(start.cpu.pc, start.cpu.a, start.cpu.x, start.cpu.y, start.cpu.p, static_cast<std::uint8_t>(start.cpu.sp));
    ref_memory.trace.clear();   // set_state's fetch of the first opcode is the caller's
    long long cycles = 0;
    int steps = 0;
    while (cpu.npc() != sentinel) {
        if (++steps > step_limit) return {Outcome::skip_runaway, {}};
        if (cpu.npc() < 0x4000) return {Outcome::skip_left, {}};   // into RAM: not the program
        // KIL: MAME reads 0xFFFF until reset. Only a state the program never
        // reaches gets here (data run as code), and nothing was lifted there.
        if (qilin::m6502::decode(cpu.ir()).flow == qilin::m6502::Flow::halt) return {Outcome::skip_left, {}};
        cycles += cpu.step();
    }

    // The lifted code.
    rt6502::Board b;
    static_cast<rt6502::Memory&>(b) = start.memory;
    b.cpu = start.cpu;
    b.start(slice, cycles + 1000);
    try {
        fn(b, 0);
        int hops = 0;
        while (b.cpu.pc != sentinel) {
            if (++hops > step_limit || b.cpu.pc < 0x4000) return {Outcome::mismatch, std::format("the lifted code went to {:04X}", b.cpu.pc)};
            b.yielding = false;
            if (!rt6502::dispatch(b)) b.unlifted();
        }
    } catch (const rt6502::Stop& stop) {
        if (stop.reason == rt6502::Stop::unlifted) return {Outcome::skip_unlifted, std::format("{:04X}", b.cpu.pc)};
        std::string where;
        for (std::size_t i = b.trace.size() > 12 ? b.trace.size() - 12 : 0; i < b.trace.size(); ++i) {
            where += "  " + event_text(b.trace[i]) + "\n";
        }
        return {Outcome::mismatch,
                std::format("the lifted code {} (MAME: {} cycles, {} bus cycles); its last bus cycles:\n{}",
                            stop.reason == rt6502::Stop::runaway ? "made far more bus cycles than the original"
                                                                 : "ran past the original's cycle count",
                            cycles, ref_memory.trace.size(), where)};
    }

    // Compare.
    std::string diff;
    const auto& r = ref_memory.trace;
    const auto& l = b.trace;
    for (std::size_t i = 0; i < std::max(r.size(), l.size()); ++i) {
        if (i >= r.size() || i >= l.size() || !(r[i] == l[i])) {
            diff += std::format("bus cycle {}: MAME {}, lifted {}\n", i, i < r.size() ? event_text(r[i]) : "(end)",
                                i < l.size() ? event_text(l[i]) : "(end)");
            if (i > 0) diff += std::format("  after {}\n", event_text(r[i - 1]));
            break;
        }
    }
    const auto reg = [&](const char* name, unsigned mame, unsigned lifted) {
        if (mame != lifted) diff += std::format("{}: MAME {:02X}, lifted {:02X}\n", name, mame, lifted);
    };
    reg("A", cpu.a(), b.cpu.a);
    reg("X", cpu.x(), b.cpu.x);
    reg("Y", cpu.y(), b.cpu.y);
    reg("P", cpu.p(), b.cpu.p);
    reg("S", cpu.s(), b.cpu.sp & 0xff);
    if (b.cycles() != cycles) diff += std::format("cycles: MAME {}, lifted {}\n", cycles, b.cycles());
    if (!diff.empty()) return {Outcome::mismatch, diff, b.yields};
    return {Outcome::match, {}, b.yields};
}

} // namespace qilin::difftest6502
