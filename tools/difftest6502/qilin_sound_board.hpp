#pragma once
// The 6502 tests' board: 64 KiB of memory and a trace of every bus cycle.
// The lifted code and MAME's 6502 core each run on one, from the same
// starting state, and their traces must match exactly.
//
// 0x0000-0x3FFF is plain memory here (devices aren't modelled: the test is
// of the CPU), and 0x4000-0xFFFF is ROM: writes there are traced and dropped.
//
// The board ends the lifted code's time slice at every service call, after a
// slice of its chosen length, so the tests also run lifted code that yields
// in the middle of functions and resumes through dispatch.

#include "m6502_rt.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace qilin::rt6502 {

enum class Access : std::uint8_t { read, arg, sync, write };

struct Event {
    Access access;
    std::uint16_t address;
    std::uint8_t value;
    bool operator==(const Event&) const = default;
};

// Ends a trial early.
struct Stop {
    enum Reason { budget, unlifted, runaway } reason;
};

struct Memory {
    std::array<std::uint8_t, 0x10000> bytes{};
    std::vector<Event> trace;

    // Far more bus cycles than any trial's original made: lifted code stuck
    // in a loop with no service check.
    static constexpr std::size_t trace_limit = 8'000'000;

    void record(const Event& e) {
        if (trace.size() >= trace_limit) throw Stop{Stop::runaway};
        trace.push_back(e);
    }
    std::uint8_t traced(Access access, std::uint16_t a) {
        const auto v = bytes[a];
        record({access, a, v});
        return v;
    }
    std::uint8_t read(std::uint16_t a) { return traced(Access::read, a); }
    std::uint8_t read_arg(std::uint16_t a) { return traced(Access::arg, a); }
    std::uint8_t read_sync(std::uint16_t a) { return traced(Access::sync, a); }
    void write(std::uint16_t a, std::uint8_t v) {
        record({Access::write, a, v});
        if (a < 0x4000) bytes[a] = v;
    }
};

struct Board : Memory {
    Cpu cpu;
    std::int32_t icount = 0;
    bool yielding = false;

    std::int32_t slice = 64;    // cycles per time slice
    long long spent = 0;        // cycles of the slices before this one
    long long budget = 0;       // stop past this many cycles
    int yields = 0;

    void start(std::int32_t slice_cycles, long long budget_cycles) {
        slice = slice_cycles;
        budget = budget_cycles;
        spent = 0;
        icount = slice;
    }
    long long cycles() const { return spent + (slice - icount); }

    void service() {
        if (icount > 0) return;
        spent += slice - icount;
        if (spent > budget) throw Stop{Stop::budget};
        icount = slice;
        yielding = true;
        ++yields;
    }
    [[noreturn]] void unlifted() { throw Stop{Stop::unlifted}; }
};

} // namespace qilin::rt6502
