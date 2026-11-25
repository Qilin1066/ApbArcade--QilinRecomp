#include "vrange.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace qilin::vrange {
namespace {

using t11::Op;
using cfg::Flow;
using cfg::Loc;

constexpr Interval top{};
constexpr Interval constant(unsigned value) noexcept {
    return {static_cast<std::int32_t>(value & 0xffff), static_cast<std::int32_t>(value & 0xffff)};
}

Interval make(std::int64_t lo, std::int64_t hi) {
    if (hi - lo >= 0xffff) return top;
    while (lo >= 0x10000) { lo -= 0x10000; hi -= 0x10000; }
    while (lo < -0x8000) { lo += 0x10000; hi += 0x10000; }
    return {static_cast<std::int32_t>(lo), static_cast<std::int32_t>(hi)};
}

// Which register the flags describe, and against what.
struct Cond {
    // bitmask: BIT #m,Rn with m a run of high bits (177740 ...): Z says Rn <= ~m, unsigned.
    enum Kind : std::uint8_t { none, compare, result, bitmask } kind = none;
    std::uint8_t reg = 0;
    std::uint16_t k = 0;      // compare: the constant; bitmask: ~m
    bool reversed = false;    // compare: flags are k - Rn (CMP #k,Rn); else Rn - k
    bool v_clear = false;     // result: V is known clear
    auto operator<=>(const Cond&) const = default;
};

struct State {
    Registers r;
    Cond cond;
    auto operator<=>(const State&) const = default;
};

State all_top() {
    State s;
    s.r.fill(top);
    return s;
}

// The values as signed or unsigned 16-bit numbers; nothing if they wrap
// past that view's ends.
struct Range {
    std::int32_t lo, hi;
};
std::optional<Range> unsigned_view(Interval i) {
    if (i.top()) return Range{0, 0xffff};
    if (i.lo >= 0 && i.hi <= 0xffff) return Range{i.lo, i.hi};
    if (i.hi < 0) return Range{i.lo + 0x10000, i.hi + 0x10000};
    return std::nullopt;
}
std::optional<Range> signed_view(Interval i) {
    if (i.top()) return Range{-0x8000, 0x7fff};
    if (i.lo >= -0x8000 && i.hi <= 0x7fff) return Range{i.lo, i.hi};
    if (i.lo >= 0x8000 && i.hi <= 0xffff) return Range{i.lo - 0x10000, i.hi - 0x10000};
    if (i.lo >= -0x18000 && i.hi <= -0x8001) return Range{i.lo + 0x10000, i.hi + 0x10000};
    return std::nullopt;
}

Interval add(Interval a, Interval b) {
    if (a.top() || b.top()) return top;
    return make(std::int64_t{a.lo} + b.lo, std::int64_t{a.hi} + b.hi);
}
Interval negate(Interval a) {
    if (a.top()) return top;
    return make(-std::int64_t{a.hi}, -std::int64_t{a.lo});
}

Interval join(Interval a, Interval b) {
    if (a.top() || b.top()) return top;
    // The same values can be written shifted by 65,536; take the tighter hull.
    Interval best = top;
    for (const std::int64_t shift : {0, 0x10000, -0x10000}) {
        const auto h = make(std::min<std::int64_t>(a.lo, b.lo + shift), std::max<std::int64_t>(a.hi, b.hi + shift));
        if (!h.top() && (best.top() || h.size() < best.size())) best = h;
    }
    return best;
}

// The value of a source operand, if it's a register or an immediate.
Interval source_value(const t11::Operand& op, const Registers& r) {
    if (op.mode == t11::reg && op.reg < 6) return r[op.reg];
    if (op.immediate()) return constant(op.word);
    return top;
}

// Effects of autoincrement and autodecrement on the registers.
void step_registers(const t11::Operand& op, bool byte, Registers& r) {
    if (op.reg >= 6) return;
    const unsigned step = (byte && (op.mode == t11::autoinc || op.mode == t11::autodec)) ? 1u : 2u;
    if (op.mode == t11::autoinc || op.mode == t11::autoinc_deferred) {
        r[op.reg] = add(r[op.reg], constant(step));
    } else if (op.mode == t11::autodec || op.mode == t11::autodec_deferred) {
        r[op.reg] = add(r[op.reg], constant(0x10000u - step));
    }
}

State transfer(const t11::Instruction& insn, State in) {
    State s = in;
    // An illegal word, or a trap: the handler runs before the next
    // instruction and may change any register, as a call may.
    if (!insn.legal || insn.op == t11::Op::emt || insn.op == t11::Op::trap || insn.op == t11::Op::bpt || insn.op == t11::Op::iot) {
        return all_top();
    }
    auto& r = s.r;
    const bool two = insn.operands == 2;
    const Interval src = two ? source_value(insn.src, in.r) : top;
    if (two) step_registers(insn.src, insn.byte, r);
    if (insn.operands >= 1) step_registers(insn.dst, insn.byte, r);

    const bool dst_reg = insn.operands >= 1 && insn.dst.mode == t11::reg && insn.dst.reg < 6;
    const unsigned d = insn.dst.reg;
    const Interval before = dst_reg ? in.r[d] : top;
    bool sets_flags = true;
    Cond cond{};

    const auto write = [&](Interval value, bool v_clear) {
        r[d] = value;
        // A byte instruction's N and Z describe the low byte only: no
        // condition on the whole register (R0 = 0x0101; DECB R0; BEQ is not R0 == 0).
        cond = insn.byte ? Cond{} : Cond{Cond::result, static_cast<std::uint8_t>(d), 0, false, v_clear};
    };
    const auto no_signed_overflow = [](Interval a, Interval b, bool subtract) {
        const auto sa = signed_view(a), sb = signed_view(b);
        if (a.top() || b.top() || !sa || !sb) return false;
        const int lo = subtract ? sa->lo - sb->hi : sa->lo + sb->lo;
        const int hi = subtract ? sa->hi - sb->lo : sa->hi + sb->hi;
        return lo >= -0x8000 && hi <= 0x7fff;
    };

    switch (insn.op) {
    case Op::mov:
        if (dst_reg) {
            if (!insn.byte) {
                write(src, true);
            } else {
                // MOVB to a register sign-extends: a value already in -128..127 is unchanged.
                const auto sv = signed_view(src);
                write(src.constant() ? constant((src.lo & 0x80) ? (0xff00u | (src.lo & 0xffu)) : (src.lo & 0xffu))
                                     : (sv && sv->lo >= -128 && sv->hi <= 127 && !src.top() ? src : top),
                      true);
            }
        }
        break;
    case Op::clr:
        if (dst_reg) {
            const auto uv = unsigned_view(before);
            write(insn.byte ? (uv && uv->hi <= 0xff ? constant(0) : top) : constant(0), true);
        }
        break;
    case Op::inc:
    case Op::dec:
        if (dst_reg && !insn.byte) {
            const bool down = insn.op == Op::dec;
            const auto value = add(before, Interval{down ? -1 : 1, down ? -1 : 1});
            // V is set only when DEC leaves -32768 or INC leaves 32767.
            const auto sv = signed_view(before);
            const bool v_clear = !before.top() && sv && (down ? sv->lo > -0x8000 : sv->hi < 0x7fff);
            write(value, v_clear);
        } else if (dst_reg) {
            write(top, false);
        }
        break;
    case Op::neg:
        if (dst_reg) write(insn.byte ? top : negate(before), false);
        break;
    case Op::asl:
        if (dst_reg) write(!insn.byte && !before.top() ? make(2 * std::int64_t{before.lo}, 2 * std::int64_t{before.hi}) : top, false);
        break;
    case Op::asr:
        if (dst_reg) {
            const auto sv = signed_view(before);
            // Arithmetic shift: floor division of the signed value.
            write(!insn.byte && !before.top() && sv ? make(sv->lo >> 1, sv->hi >> 1) : top, false);
        }
        break;
    case Op::add:
    case Op::sub:
        if (dst_reg) {
            const bool subtract = insn.op == Op::sub;
            write(subtract ? add(before, negate(src)) : add(before, src), no_signed_overflow(before, src, subtract));
        }
        break;
    case Op::bic:
        if (dst_reg) {
            const auto kept = static_cast<std::int32_t>((insn.byte ? 0xff00u | ~static_cast<unsigned>(src.lo)
                                                                   : ~static_cast<unsigned>(src.lo)) & 0xffffu);
            const auto uv = unsigned_view(before);
            write(src.constant() ? Interval{0, uv ? std::min(uv->hi, kept) : kept} : top, true);
        }
        break;
    case Op::bis:
        if (dst_reg) {
            // OR can't set a bit above the highest either operand may have.
            const auto ub = unsigned_view(before), us = unsigned_view(src);
            std::int32_t limit = 0xffff;
            if (insn.byte && ub && ub->hi <= 0xff) limit = 0xff;   // only the low byte changes
            else if (!insn.byte && ub && us) {
                const auto high = std::max(ub->hi, us->hi);
                limit = 1;
                while (limit < high) limit = limit * 2 + 1;
            }
            write(before.constant() && src.constant() ? constant(static_cast<unsigned>(before.lo | src.lo))
                                                      : (limit < 0xffff ? Interval{0, limit} : top),
                  true);
        }
        break;
    case Op::tst:
        if (dst_reg && !insn.byte) cond = {Cond::result, static_cast<std::uint8_t>(d), 0, false, true};
        break;
    case Op::bit:
        if (dst_reg && !insn.byte && insn.src.immediate()) {
            const auto low = static_cast<std::uint16_t>(~insn.src.word);
            if (low != 0xffff && (low & (low + 1u)) == 0) {   // the mask is all the bits above 'low'
                cond = {Cond::bitmask, static_cast<std::uint8_t>(d), low, false, false};
            }
        }
        break;
    case Op::cmp:
        if (!insn.byte) {
            if (insn.src.immediate() && dst_reg) {
                cond = {Cond::compare, static_cast<std::uint8_t>(d), insn.src.word, true, false};
            } else if (insn.src.mode == t11::reg && insn.src.reg < 6 && insn.dst.immediate()) {
                cond = {Cond::compare, insn.src.reg, insn.dst.word, false, false};
            }
        }
        break;
    case Op::br: case Op::bne: case Op::beq: case Op::bge: case Op::blt: case Op::bgt: case Op::ble:
    case Op::bpl: case Op::bmi: case Op::bhi: case Op::blos: case Op::bvc: case Op::bvs: case Op::bcc: case Op::bcs:
    case Op::jmp: case Op::jsr: case Op::wait:
        sets_flags = false;
        break;
    case Op::sob:
        sets_flags = false;
        if (insn.reg < 6) {
            r[insn.reg] = add(in.r[insn.reg], constant(0xffff));
            if (in.cond.reg == insn.reg) s.cond = {};
        }
        return s;
    case Op::rts:
        if (insn.reg < 6) r[insn.reg] = top;
        sets_flags = false;
        break;
    default:
        if (dst_reg && insn.op != Op::bit && insn.op != Op::mtps) r[d] = top;
        if (insn.op == Op::xor_ && insn.dst.mode == t11::reg && insn.dst.reg < 6) r[insn.dst.reg] = top;
        break;
    }
    if (sets_flags) {
        s.cond = cond;
    } else if (s.cond.kind != Cond::none && (s.r[s.cond.reg] != in.r[s.cond.reg] || insn.op == Op::jsr)) {
        s.cond = {};
    }
    return s;
}

// Narrows the state for the edge of a conditional branch; nothing if the
// edge can't be taken.
std::optional<State> narrow(const State& in, Op op, bool taken) {
    const auto& c = in.cond;
    if (c.kind == Cond::none) return in;
    State s = in;
    auto& v = s.r[c.reg];
    // Express the condition as "Rn REL k".
    enum Rel { eq, ne, ult, ule, ugt, uge, slt, sle, sgt, sge, unknown } rel = unknown;
    const auto flip = [](Rel x) {   // swap operands
        switch (x) {
        case ult: return ugt; case ule: return uge; case ugt: return ult; case uge: return ule;
        case slt: return sgt; case sle: return sge; case sgt: return slt; case sge: return sle;
        default: return x;
        }
    };
    const auto negate_rel = [](Rel x) {
        switch (x) {
        case eq: return ne; case ne: return eq; case ult: return uge; case uge: return ult;
        case ule: return ugt; case ugt: return ule; case slt: return sge; case sge: return slt;
        case sle: return sgt; case sgt: return sle; default: return x;
        }
    };
    std::uint16_t k = c.k;
    if (c.kind == Cond::compare) {
        switch (op) {
        case Op::beq: rel = eq; break;
        case Op::bne: rel = ne; break;
        case Op::bhi: rel = ugt; break;
        case Op::blos: rel = ule; break;
        case Op::bcc: rel = uge; break;
        case Op::bcs: rel = ult; break;
        case Op::bge: rel = sge; break;
        case Op::blt: rel = slt; break;
        case Op::bgt: rel = sgt; break;
        case Op::ble: rel = sle; break;
        default: break;
        }
        if (c.reversed) rel = flip(rel);   // k REL Rn  ==  Rn flip(REL) k
    } else if (c.kind == Cond::bitmask) {
        if (op == Op::beq) rel = ule;      // no bit of the mask set
        if (op == Op::bne) rel = ugt;
    } else {
        k = 0;
        switch (op) {
        case Op::beq: rel = eq; break;
        case Op::bne: rel = ne; break;
        case Op::bpl: rel = sge; break;
        case Op::bmi: rel = slt; break;
        case Op::bge: if (c.v_clear) rel = sge; break;
        case Op::blt: if (c.v_clear) rel = slt; break;
        case Op::bgt: if (c.v_clear) rel = sgt; break;
        case Op::ble: if (c.v_clear) rel = sle; break;
        default: break;
        }
    }
    if (rel == unknown) return in;
    if (!taken) rel = negate_rel(rel);

    const int sk = k >= 0x8000 ? k - 0x10000 : k;
    switch (rel) {
    case eq: {
        // Is k among the values?
        bool member = v.top();
        for (const std::int32_t shift : {0, 0x10000, -0x10000}) member = member || (k + shift >= v.lo && k + shift <= v.hi);
        if (!member) return std::nullopt;
        v = constant(k);
        break;
    }
    case ne:
        if (v.constant() && (v.lo & 0xffff) == k) return std::nullopt;
        if (!v.top() && (v.lo & 0xffff) == k) ++v.lo;
        else if (!v.top() && (v.hi & 0xffff) == k) --v.hi;
        break;
    case ult: case ule: case ugt: case uge: {
        const auto bound = [&](Range r) {
            if (rel == ult) r.hi = std::min<std::int32_t>(r.hi, k - 1);
            if (rel == ule) r.hi = std::min<std::int32_t>(r.hi, k);
            if (rel == ugt) r.lo = std::max<std::int32_t>(r.lo, k + 1);
            if (rel == uge) r.lo = std::max<std::int32_t>(r.lo, k);
            return r;
        };
        if (const auto uv = unsigned_view(v)) {
            const auto r = bound(*uv);
            if (r.lo > r.hi) return std::nullopt;
            v = make(r.lo, r.hi);
            break;
        }
        // The values wrap past 0xFFFF to 0 (a byte minus a constant, say):
        // narrow the high run and the low run apart, then rejoin them.
        const Range high = bound(v.lo < 0 ? Range{v.lo + 0x10000, 0xffff} : Range{v.lo, 0xffff});
        const Range low = bound(v.lo < 0 ? Range{0, v.hi} : Range{0, v.hi - 0x10000});
        const bool has_high = high.lo <= high.hi, has_low = low.lo <= low.hi;
        if (!has_high && !has_low) return std::nullopt;
        v = has_high && has_low ? make(high.lo - 0x10000, low.hi) : has_low ? make(low.lo, low.hi) : make(high.lo, high.hi);
        break;
    }
    case slt: case sle: case sgt: case sge: {
        const auto sv = signed_view(v);
        if (!sv) return s;
        auto r = *sv;
        if (rel == slt) r.hi = std::min(r.hi, sk - 1);
        if (rel == sle) r.hi = std::min(r.hi, sk);
        if (rel == sgt) r.lo = std::max(r.lo, sk + 1);
        if (rel == sge) r.lo = std::max(r.lo, sk);
        if (r.lo > r.hi) return std::nullopt;
        v = make(r.lo, r.hi);
        break;
    }
    default: break;
    }
    if (v.lo > v.hi) return std::nullopt;
    return s;
}

bool is_conditional(Op op) {
    switch (op) {
    case Op::bne: case Op::beq: case Op::bge: case Op::blt: case Op::bgt: case Op::ble:
    case Op::bpl: case Op::bmi: case Op::bhi: case Op::blos: case Op::bvc: case Op::bvs: case Op::bcc: case Op::bcs:
        return true;
    default:
        return false;
    }
}

} // namespace

std::map<Loc, Registers> analyze(const cfg::Program& program) {
    const auto& nodes = program.nodes();
    std::map<Loc, State> in;
    std::map<Loc, unsigned> visits;
    std::vector<Loc> work;
    const auto merge = [&](Loc at, const State& state) {
        if (!nodes.contains(at)) return;
        auto [it, inserted] = in.try_emplace(at, state);
        if (!inserted) {
            State joined = it->second;
            for (std::size_t i = 0; i < joined.r.size(); ++i) joined.r[i] = join(joined.r[i], state.r[i]);
            if (joined.cond != state.cond) joined.cond = {};
            if (joined == it->second) return;
            // Widen a location that keeps changing.
            if (++visits[at] > 32) {
                for (std::size_t i = 0; i < joined.r.size(); ++i) {
                    if (joined.r[i] != it->second.r[i]) joined.r[i] = top;
                }
            }
            it->second = joined;
        }
        work.push_back(at);
    };
    for (const auto& entry : program.functions()) merge(entry, all_top());
    for (const auto& site : program.return_sites()) merge(site, all_top());

    while (!work.empty()) {
        const auto at = work.back();
        work.pop_back();
        const auto& node = nodes.at(at);
        const State state = in.at(at);
        const State out = transfer(node.insn, state);
        const bool call = node.flow == Flow::call || node.flow == Flow::call_dynamic;
        for (std::size_t i = 0; i < node.next.size(); ++i) {
            const auto& successor = node.next[i];
            if (call) {
                merge(successor, all_top());   // the callee may change any register
            } else if (is_conditional(node.insn.op)) {
                // The CFG lists a branch's target first, when it could place it.
                const bool taken = i == 0 && node.target && successor == *node.target;
                if (const auto edge = narrow(state, node.insn.op, taken)) {
                    State edge_out = *edge;
                    edge_out.cond = {};
                    merge(successor, edge_out);
                }
            } else {
                merge(successor, out);
            }
        }
    }
    std::map<Loc, Registers> result;
    for (const auto& [loc, state] : in) result.emplace(loc, state.r);
    return result;
}

} // namespace qilin::vrange
