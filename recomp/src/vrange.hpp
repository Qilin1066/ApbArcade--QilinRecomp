#pragma once
// Value-range analysis of the T-11 registers, for sizing jump tables.
//
// Each of R0-R5 holds an unsigned interval. Instructions update it (constant
// moves, register copies, CLR, INC, DEC, NEG, ASL, ASR, ADD and SUB of a
// constant or a register, BIC with a constant mask), and a conditional branch
// narrows the register the last comparison tested (CMP with a constant, TST,
// or the result of the instruction that set the flags), on each edge,
// signed and unsigned conditions alike. Anything it can't model — memory,
// calls, autoincrement of an unknown register — makes the register unknown,
// so a result is never narrower than the truth.

#include "cfg.hpp"

#include <array>
#include <cstdint>
#include <map>

namespace qilin::vrange {

// The values lo..hi taken modulo 65,536, so [-1, 2] is 0xFFFF, 0, 1, 2:
// arithmetic stays exact through wrap-around. lo is kept in
// [-0x8000, 0xFFFF], and a span of 65,536 values or more is "unknown".
struct Interval {
    std::int32_t lo = 0, hi = 0xffff;
    [[nodiscard]] bool top() const noexcept { return hi - lo >= 0xffff; }
    [[nodiscard]] bool constant() const noexcept { return lo == hi; }
    [[nodiscard]] std::int32_t size() const noexcept { return hi - lo + 1; }
    auto operator<=>(const Interval&) const = default;
};

using Registers = std::array<Interval, 6>;   // R0-R5; SP and PC aren't tracked

// The register ranges on entry to every reached instruction.
[[nodiscard]] std::map<cfg::Loc, Registers> analyze(const cfg::Program& program);

} // namespace qilin::vrange
