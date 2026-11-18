#include "m6502_cfg.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <deque>
#include <format>
#include <optional>

namespace qilin::m6502 {

namespace {

// ---- Abstract values ----------------------------------------------------

using ByteSet = std::bitset<256>;

ByteSet all_bytes() { return ByteSet{}.set(); }

ByteSet only(int value) {
    ByteSet set;
    set.set(static_cast<std::size_t>(value & 0xff));
    return set;
}

ByteSet span_of(int lo, int hi) {
    ByteSet set;
    for (int v = std::max(lo, 0); v <= std::min(hi, 255); ++v) {
        set.set(static_cast<std::size_t>(v));
    }
    return set;
}

template <typename F>
void each(const ByteSet& set, F f) {
    for (int v = 0; v < 256; ++v) {
        if (set[static_cast<std::size_t>(v)]) {
            f(v);
        }
    }
}

int single(const ByteSet& set) {
    for (int v = 0; v < 256; ++v) {
        if (set[static_cast<std::size_t>(v)]) {
            return v;
        }
    }
    return -1;
}

enum class Tri : std::uint8_t { no, yes, unknown };

Tri join(Tri a, Tri b) { return a == b ? a : Tri::unknown; }

// Collects a flag's value over several possibilities.
class TriSet {
public:
    void add(bool value) { (value ? yes_ : no_) = true; }
    [[nodiscard]] Tri tri() const { return yes_ && !no_ ? Tri::yes : no_ && !yes_ ? Tri::no : Tri::unknown; }

private:
    bool yes_ = false, no_ = false;
};

Tri negative(const ByteSet& set) {
    const bool high = (set & span_of(0x80, 0xff)).any();
    const bool low = (set & span_of(0, 0x7f)).any();
    return high && !low ? Tri::yes : low && !high ? Tri::no : Tri::unknown;
}

Tri zero(const ByteSet& set) {
    return !set[0] ? Tri::no : set.count() == 1 ? Tri::yes : Tri::unknown;
}

// A byte value: the set of values it can have, which instance it is, and,
// when it was computed from another instance (an index, say), its value for
// each value of that one. Narrowing any value narrows the instance it
// depends on and with it every other value depending on that instance, so
// "Y such that TABLE[Y] < 15" is known exactly.
//
// An instance is named by the instruction that made it (pc * 4 + slot), and
// the names are kept unique within a state: when an instruction makes a new
// instance, older values with its name lose their links.
struct Value {
    ByteSet set = all_bytes();
    int id = -1;     // the instance this value is; -1 when it can't be linked
    int root = -1;   // the instance it is a function of; -1 for none
    std::array<std::int16_t, 256> map{};   // root >= 0: value for each root value, -1 where impossible

    bool operator==(const Value& other) const {
        return set == other.set && id == other.id && root == other.root && (root < 0 || map == other.map);
    }
};

Value constant(int v) {
    Value value;
    value.set = only(v);
    return value;
}

Value join(const Value& a, const Value& b) {
    Value value;
    value.set = a.set | b.set;
    value.id = a.id == b.id ? a.id : -1;
    if (a.root >= 0 && a.root == b.root) {
        value.root = a.root;
        for (std::size_t i = 0; i < 256; ++i) {
            const auto x = a.map[i];
            const auto y = b.map[i];
            if (x >= 0 && y >= 0 && x != y) {
                value.root = -1;   // different functions of the same instance
                break;
            }
            value.map[i] = x >= 0 ? x : y;
        }
    }
    return value;
}

// The instance a value can be narrowed through, and its value for a given value of that instance.
int root_of(const Value& v) { return v.root >= 0 ? v.root : v.id; }
int value_at(const Value& v, int index) {
    if (v.root >= 0) {
        return v.map[static_cast<std::size_t>(index)];
    }
    return v.set[static_cast<std::size_t>(index)] ? index : -1;
}

// f(v) for every value of v, keeping the link to v's instance. f returns -1
// when it can't tell (reading RAM, say): then the result is unknown.
template <typename F>
Value derive(const Value& v, F f) {
    Value out;
    out.set.reset();
    out.root = root_of(v);
    for (int i = 0; i < 256; ++i) {
        const int x = out.root >= 0 ? value_at(v, i) : (v.set[static_cast<std::size_t>(i)] ? i : -1);
        if (x < 0) {
            out.map[static_cast<std::size_t>(i)] = -1;
            continue;
        }
        const int y = f(x);
        if (y < 0) {
            return Value{};
        }
        out.map[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(y & 0xff);
        out.set.set(static_cast<std::size_t>(y & 0xff));
    }
    return out;
}

// ---- Machine state --------------------------------------------------------

// What N and Z (and, for a comparison, C) were last computed from.
struct Source {
    enum class Kind : std::uint8_t { none, value, compare };
    Kind kind = Kind::none;
    std::uint8_t reg = 0;    // A, X, Y
    int operand = 0;         // compare: the constant compared with
    bool carry = false;      // compare: C still holds the comparison's carry
    bool operator==(const Source&) const = default;
};

constexpr int A = 0, X = 1, Y = 2;

struct State {
    Value r[3];
    Tri n = Tri::unknown, z = Tri::unknown, c = Tri::unknown, v = Tri::unknown, d = Tri::unknown;
    Source src;
    bool stack_known = true;      // the bytes pushed since the function's entry are listed
    std::vector<Value> stack;
    bool operator==(const State&) const = default;

    template <typename F>
    bool all_values(F f) {
        for (Value& value : r) {
            if (!f(value)) {
                return false;
            }
        }
        for (Value& value : stack) {
            if (!f(value)) {
                return false;
            }
        }
        return true;
    }
};

State join(const State& a, const State& b) {
    State s;
    for (int i = 0; i < 3; ++i) {
        s.r[i] = join(a.r[i], b.r[i]);
    }
    s.n = join(a.n, b.n);
    s.z = join(a.z, b.z);
    s.c = join(a.c, b.c);
    s.v = join(a.v, b.v);
    s.d = join(a.d, b.d);
    s.src = a.src == b.src ? a.src : Source{};
    if (a.stack_known && b.stack_known && a.stack.size() == b.stack.size()) {
        s.stack.resize(a.stack.size());
        for (std::size_t i = 0; i < a.stack.size(); ++i) {
            s.stack[i] = join(a.stack[i], b.stack[i]);
        }
    } else {
        s.stack_known = false;
    }
    return s;
}

// Makes v the instance named id: older values with that name lose their links.
Value fresh(State& s, Value v, int id) {
    s.all_values([&](Value& u) {
        if (u.root == id) {
            u.root = -1;
        }
        if (u.id == id) {
            u.id = -1;
        }
        return true;
    });
    if (v.root == id) {
        v.root = -1;
    }
    v.id = id;
    return v;
}

// Keeps only the executions where the instance 'root' has a value in 'allowed'.
bool restrict_root(State& s, int root, const ByteSet& allowed) {
    return s.all_values([&](Value& u) {
        if (u.root == root) {
            ByteSet image;
            for (std::size_t i = 0; i < 256; ++i) {
                if (u.map[i] < 0) {
                    continue;
                }
                if (!allowed[i]) {
                    u.map[i] = -1;
                } else {
                    image.set(static_cast<std::size_t>(u.map[i]));
                }
            }
            u.set = image;
        } else if (u.root < 0 && u.id == root) {
            u.set &= allowed;
        }
        return u.set.any();
    });
}

// Keeps only the executions where register reg's value is in 'allowed'.
bool restrict_reg(State& s, int reg, const ByteSet& allowed) {
    const Value& v = s.r[reg];
    const ByteSet kept = v.set & allowed;
    if (kept.none()) {
        return false;
    }
    if (kept == v.set) {
        return true;
    }
    const int root = root_of(v);
    if (root < 0) {
        s.r[reg].set = kept;
        return true;
    }
    ByteSet indices;
    for (int i = 0; i < 256; ++i) {
        const int x = value_at(v, i);
        if (x >= 0 && allowed[static_cast<std::size_t>(x)]) {
            indices.set(static_cast<std::size_t>(i));
        }
    }
    return restrict_root(s, root, indices);
}

// What running one instruction did, besides the new state.
struct Effects {
    bool underflow = false;   // pulled a byte the function didn't push
};

constexpr std::size_t max_pairs = 4096;

} // namespace

// ---- The interpreter ------------------------------------------------------

struct Interpreter {
    // A state is kept per address and per number of bytes the function has
    // pushed there (-1: not known), so a return and a jump through pushed
    // bytes sharing one RTS stay apart.
    using Key = std::pair<std::uint16_t, int>;
    static constexpr std::size_t max_depths = 4;

    Program& program;
    std::map<Key, State> states;
    std::map<std::uint16_t, std::set<int>> depths;
    std::map<Key, int> visits;
    std::deque<Key> work;
    std::set<Key> queued;

    explicit Interpreter(Program& p) : program(p) {}

    std::uint8_t rom(std::uint32_t address) const { return program.byte(address & 0xffffu); }

    static int id_of(std::uint16_t pc, int slot) { return pc * 4 + slot; }

    // The value an instruction reads as its operand.
    Value operand(const Instruction& insn, const State& s) const {
        switch (insn.mode) {
        case Mode::imm:
            return constant(insn.operand);
        case Mode::aba:
            return insn.operand >= Program::rom_start ? constant(rom(insn.operand)) : Value{};
        case Mode::abx:
        case Mode::aby: {
            const unsigned base = insn.operand;
            return derive(s.r[insn.mode == Mode::abx ? X : Y], [&](int i) -> int {
                const unsigned address = base + static_cast<unsigned>(i);
                return base >= Program::rom_start && address <= 0xffff ? rom(address) : -1;
            });
        }
        default:
            return Value{};   // RAM and devices
        }
    }

    static void flags_from(State& s, int reg) {
        s.n = negative(s.r[reg].set);
        s.z = zero(s.r[reg].set);
        s.src = {Source::Kind::value, static_cast<std::uint8_t>(reg), 0, false};
    }

    // Loads a register with a new instance and sets N and Z from it.
    static void load(State& s, int reg, Value value, int id) {
        s.r[reg] = fresh(s, std::move(value), id);
        flags_from(s, reg);
    }

    static void unknown_flags(State& s) {
        s.n = s.z = s.c = s.v = Tri::unknown;
        s.src = {};
    }

    // reg = f(reg, carry in), carry out from f too.
    template <typename F>
    static void shift(State& s, int reg, int id, F f) {
        TriSet carry;
        each(s.r[reg].set, [&](int value) {
            for (int cin = 0; cin < 2; ++cin) {
                if ((cin == 0 && s.c == Tri::yes) || (cin == 1 && s.c == Tri::no)) {
                    continue;
                }
                carry.add(f(value, cin).second != 0);
            }
        });
        Value result;
        if (s.c != Tri::unknown) {
            const int cin = s.c == Tri::yes ? 1 : 0;
            result = derive(s.r[reg], [&](int value) { return f(value, cin).first & 0xff; });
        } else {
            result.set.reset();
            each(s.r[reg].set, [&](int value) {
                for (int cin = 0; cin < 2; ++cin) {
                    result.set.set(static_cast<std::size_t>(f(value, cin).first & 0xff));
                }
            });
        }
        const Tri c = carry.tri();
        load(s, reg, std::move(result), id);
        s.c = c;
    }

    // Executes everything but control flow.
    Effects execute(std::uint16_t pc, const Instruction& insn, State& s) const {
        Effects effects;
        const Value m = operand(insn, s);
        const int m_value = m.set.count() == 1 ? single(m.set) : -1;
        const auto pop = [&]() -> Value {
            if (!s.stack_known) {
                return {};
            }
            if (s.stack.empty()) {
                effects.underflow = true;
                s.stack_known = false;
                return {};
            }
            Value value = s.stack.back();
            s.stack.pop_back();
            return value;
        };
        // A = f(A, m): a function of A when m is a constant.
        const auto binary = [&](auto f) {
            Value result;
            if (m_value >= 0) {
                result = derive(s.r[A], [&](int x) { return f(x, m_value) & 0xff; });
            } else if (s.r[A].set.count() * m.set.count() <= max_pairs) {
                result.set.reset();
                each(s.r[A].set, [&](int x) {
                    each(m.set, [&](int y) { result.set.set(static_cast<std::size_t>(f(x, y) & 0xff)); });
                });
            }
            load(s, A, std::move(result), id_of(pc, A));
        };
        const auto arithmetic = [&](bool subtract) {
            const auto sum = [&](int x, int y, int cin) { return subtract ? x - y - (1 - cin) : x + y + cin; };
            const auto carries = [&](int total) { return subtract ? total >= 0 : total > 0xff; };
            if (s.d != Tri::no || s.r[A].set.count() * m.set.count() * 2 > max_pairs) {
                load(s, A, Value{}, id_of(pc, A));
                s.c = s.v = Tri::unknown;
                return;
            }
            TriSet carry;
            Value result;
            result.set.reset();
            each(s.r[A].set, [&](int x) {
                each(m.set, [&](int y) {
                    for (int cin = 0; cin < 2; ++cin) {
                        if ((cin == 0 && s.c == Tri::yes) || (cin == 1 && s.c == Tri::no)) {
                            continue;
                        }
                        result.set.set(static_cast<std::size_t>(sum(x, y, cin) & 0xff));
                        carry.add(carries(sum(x, y, cin)));
                    }
                });
            });
            if (m_value >= 0 && s.c != Tri::unknown) {
                const int cin = s.c == Tri::yes ? 1 : 0;
                result = derive(s.r[A], [&](int x) { return sum(x, m_value, cin) & 0xff; });
            }
            const Tri c = carry.tri();
            load(s, A, std::move(result), id_of(pc, A));
            s.c = c;
            s.v = Tri::unknown;
        };
        const auto compare = [&](int reg) {
            const ByteSet& r = s.r[reg].set;
            if (r.count() * m.set.count() > max_pairs) {
                unknown_flags(s);
                return;
            }
            TriSet n, z, c;
            each(r, [&](int x) {
                each(m.set, [&](int y) {
                    n.add(((x - y) & 0x80) != 0);
                    z.add(x == y);
                    c.add(x >= y);
                });
            });
            s.n = n.tri();
            s.z = z.tri();
            s.c = c.tri();
            s.src = m_value >= 0 ? Source{Source::Kind::compare, static_cast<std::uint8_t>(reg), m_value, true} : Source{};
        };
        const auto memory_rmw = [&](bool sets_carry) {
            s.n = s.z = Tri::unknown;
            if (sets_carry) {
                s.c = Tri::unknown;
            }
            s.src = {};
        };
        const auto step = [&](int reg, int delta) {
            load(s, reg, derive(s.r[reg], [&](int x) { return (x + delta) & 0xff; }), id_of(pc, reg));
        };
        const auto copy = [&](int to, int from) {
            s.r[to] = s.r[from];
            flags_from(s, to);
        };

        switch (insn.op) {
        case Op::lda: load(s, A, m, id_of(pc, A)); break;
        case Op::ldx: load(s, X, m, id_of(pc, X)); break;
        case Op::ldy: load(s, Y, m, id_of(pc, Y)); break;
        case Op::tax: copy(X, A); break;
        case Op::tay: copy(Y, A); break;
        case Op::txa: copy(A, X); break;
        case Op::tya: copy(A, Y); break;
        case Op::tsx: load(s, X, Value{}, id_of(pc, X)); break;
        case Op::txs: s.stack_known = false; s.stack.clear(); break;
        case Op::inx: step(X, 1); break;
        case Op::iny: step(Y, 1); break;
        case Op::dex: step(X, -1); break;
        case Op::dey: step(Y, -1); break;
        case Op::and_: binary([](int x, int y) { return x & y; }); break;
        case Op::ora: binary([](int x, int y) { return x | y; }); break;
        case Op::eor: binary([](int x, int y) { return x ^ y; }); break;
        case Op::adc: arithmetic(false); break;
        case Op::sbc: arithmetic(true); break;
        case Op::cmp: compare(A); break;
        case Op::cpx: compare(X); break;
        case Op::cpy: compare(Y); break;
        case Op::bit: {
            if (s.r[A].set.count() * m.set.count() > max_pairs) {
                unknown_flags(s);
                break;
            }
            TriSet z, n, v;
            each(s.r[A].set, [&](int x) {
                each(m.set, [&](int y) {
                    z.add((x & y) == 0);
                    n.add((y & 0x80) != 0);
                    v.add((y & 0x40) != 0);
                });
            });
            s.n = n.tri();
            s.z = z.tri();
            s.v = v.tri();
            s.src = {};
            break;
        }
        case Op::asl:
            if (insn.mode == Mode::acc) {
                shift(s, A, id_of(pc, A), [](int value, int) { return std::pair{value << 1, value >> 7}; });
            } else {
                memory_rmw(true);
            }
            break;
        case Op::lsr:
            if (insn.mode == Mode::acc) {
                shift(s, A, id_of(pc, A), [](int value, int) { return std::pair{value >> 1, value & 1}; });
            } else {
                memory_rmw(true);
            }
            break;
        case Op::rol:
            if (insn.mode == Mode::acc) {
                shift(s, A, id_of(pc, A), [](int value, int cin) { return std::pair{(value << 1) | cin, value >> 7}; });
            } else {
                memory_rmw(true);
            }
            break;
        case Op::ror:
            if (insn.mode == Mode::acc) {
                shift(s, A, id_of(pc, A), [](int value, int cin) { return std::pair{(value >> 1) | (cin << 7), value & 1}; });
            } else {
                memory_rmw(true);
            }
            break;
        case Op::inc:
        case Op::dec:
            memory_rmw(false);
            break;
        case Op::clc:
            s.c = Tri::no;
            s.src.carry = false;
            break;
        case Op::sec:
            s.c = Tri::yes;
            s.src.carry = false;
            break;
        case Op::clv: s.v = Tri::no; break;
        case Op::cld: s.d = Tri::no; break;
        case Op::sed: s.d = Tri::yes; break;
        case Op::cli: case Op::sei: case Op::nop:
        case Op::sta: case Op::stx: case Op::sty:
            break;
        case Op::pha:
            if (s.stack_known) {
                s.stack.push_back(s.r[A]);
            }
            break;
        case Op::php:
            if (s.stack_known) {
                s.stack.push_back(Value{});
            }
            break;
        case Op::pla: {
            Value value = pop();
            s.r[A] = value.id < 0 && value.root < 0 ? fresh(s, std::move(value), id_of(pc, A)) : std::move(value);
            flags_from(s, A);
            break;
        }
        case Op::plp:
            pop();
            unknown_flags(s);
            s.d = Tri::unknown;
            break;
        case Op::jmp: case Op::jsr: case Op::rts: case Op::rti: case Op::brk: case Op::kil:
        case Op::bcc: case Op::bcs: case Op::beq: case Op::bmi: case Op::bne: case Op::bpl:
        case Op::bvc: case Op::bvs:
            break;
        default:
            // Undocumented: what each changes is not modelled, so forget it all.
            for (int reg = 0; reg < 3; ++reg) {
                s.r[reg] = fresh(s, Value{}, id_of(pc, reg));
            }
            unknown_flags(s);
            if (insn.op == Op::shs || insn.op == Op::las) {
                s.stack_known = false;
                s.stack.clear();
            }
            break;
        }
        return effects;
    }

    // The state on one edge of a branch, or nothing if no execution takes it.
    static std::optional<State> narrow(const Instruction& insn, State s, bool taken) {
        Tri* flag = nullptr;
        bool want = false;
        switch (insn.op) {
        case Op::bpl: flag = &s.n; want = !taken; break;
        case Op::bmi: flag = &s.n; want = taken; break;
        case Op::bvc: flag = &s.v; want = !taken; break;
        case Op::bvs: flag = &s.v; want = taken; break;
        case Op::bcc: flag = &s.c; want = !taken; break;
        case Op::bcs: flag = &s.c; want = taken; break;
        case Op::bne: flag = &s.z; want = !taken; break;
        case Op::beq: flag = &s.z; want = taken; break;
        default: return s;
        }
        if (*flag != Tri::unknown && (*flag == Tri::yes) != want) {
            return std::nullopt;
        }
        const bool tests_n = flag == &s.n;
        const bool tests_z = flag == &s.z;
        const bool tests_c = flag == &s.c;
        *flag = want ? Tri::yes : Tri::no;

        const int reg = s.src.reg;
        if (s.src.kind == Source::Kind::value && (tests_n || tests_z)) {
            const ByteSet allowed = tests_z ? (want ? only(0) : ~only(0)) : (want ? span_of(0x80, 0xff) : span_of(0, 0x7f));
            if (!restrict_reg(s, reg, allowed)) {
                return std::nullopt;
            }
            s.n = negative(s.r[reg].set);
            s.z = zero(s.r[reg].set);
        } else if (s.src.kind == Source::Kind::compare && (tests_z || (tests_c && s.src.carry))) {
            const int k = s.src.operand;
            const ByteSet allowed = tests_z ? (want ? only(k) : ~only(k)) : (want ? span_of(k, 0xff) : span_of(0, k - 1));
            if (!restrict_reg(s, reg, allowed)) {
                return std::nullopt;
            }
            TriSet n, z, c;
            each(s.r[reg].set, [&](int x) {
                n.add(((x - k) & 0x80) != 0);
                z.add(x == k);
                c.add(x >= k);
            });
            s.n = n.tri();
            s.z = z.tri();
            if (s.src.carry) {
                s.c = c.tri();
            }
        }
        return s;
    }

    struct Outcome {
        std::vector<std::pair<std::uint16_t, State>> edges;   // within the function
        std::optional<std::uint16_t> call;                    // a callee's entry
        std::optional<State> callee;                          // and the state the call enters it with
        Transfer transfer = Transfer::next;
        std::uint16_t target = 0;
        std::vector<std::uint16_t> table;
        bool never_taken = false;
        bool always_taken = false;
        std::vector<Finding> findings;
    };

    // An RTS through the two bytes on top of the pushed list.
    void rts_through_pushes(std::uint16_t pc, const State& s, Outcome& out) const {
        const Value& lo = s.stack.back();
        const Value& hi = s.stack[s.stack.size() - 2];
        State after = s;
        after.stack.resize(after.stack.size() - 2);
        const auto target_of = [](int l, int h) { return static_cast<std::uint16_t>(((h << 8) | l) + 1); };
        const int l1 = lo.set.count() == 1 ? single(lo.set) : -1;
        const int h1 = hi.set.count() == 1 ? single(hi.set) : -1;
        if (l1 >= 0 && h1 >= 0) {
            out.transfer = Transfer::table;
            out.table.push_back(target_of(l1, h1));
            out.edges.emplace_back(out.table.back(), std::move(after));
            return;
        }
        // Both bytes must be functions of one instance (or constant).
        const int root = l1 < 0 ? root_of(lo) : root_of(hi);
        if (root < 0 || (l1 < 0 && root_of(lo) != root) || (h1 < 0 && root_of(hi) != root)) {
            out.transfer = Transfer::computed_jump;
            out.findings.push_back({Issue::computed_jump, pc, "RTS through pushed bytes of unknown origin"});
            return;
        }
        // The index's values are exactly those the analysis allows here, so
        // every target is listed; slots that aren't code are left out.
        std::map<std::uint16_t, ByteSet> by_target;
        for (int i = 0; i < 256; ++i) {
            const int l = l1 >= 0 ? l1 : value_at(lo, i);
            const int h = h1 >= 0 ? h1 : value_at(hi, i);
            if (l >= 0 && h >= 0) {
                by_target[target_of(l, h)].set(static_cast<std::size_t>(i));
            }
        }
        out.transfer = Transfer::table;
        for (const auto& [target, allowed] : by_target) {
            if (!plausible_code(target)) {
                out.findings.push_back({Issue::implausible_entry, pc,
                                        std::format("{:04X}, for {} of the index's values", target, allowed.count())});
                continue;
            }
            State edge = after;
            if (restrict_root(edge, root, allowed)) {
                out.table.push_back(target);
                out.edges.emplace_back(target, std::move(edge));
            }
        }
    }

    // Everything that follows from running the instruction at pc in state s.
    Outcome follow(std::uint16_t pc, const Instruction& insn, const State& in) const {
        Outcome out;
        State s = in;
        const Effects effects = execute(pc, insn, s);
        if (effects.underflow) {
            out.findings.push_back({Issue::pops_return, pc, disassemble(insn, pc) + " pulls a byte the function didn't push"});
        }
        if (!insn.documented) {
            out.findings.push_back({Issue::undocumented, pc, disassemble(insn, pc)});
        }
        const auto next = insn.next(pc);
        switch (insn.flow) {
        case Flow::next:
            out.edges.emplace_back(next, s);
            break;
        case Flow::branch: {
            out.transfer = Transfer::branch;
            out.target = insn.branch_target(pc);
            auto taken = narrow(insn, s, true);
            auto fall = narrow(insn, s, false);
            out.always_taken = !fall;
            out.never_taken = !taken;
            if (taken) {
                out.edges.emplace_back(out.target, std::move(*taken));
            }
            if (fall) {
                out.edges.emplace_back(next, std::move(*fall));
            }
            break;
        }
        case Flow::jump:
            out.transfer = Transfer::jump;
            out.target = insn.operand;
            out.edges.emplace_back(out.target, s);
            break;
        case Flow::call: {
            out.transfer = Transfer::call;
            out.target = insn.operand;
            out.call = insn.operand;
            // The callee starts with the caller's registers and flags, and nothing pushed.
            out.callee = s;
            out.callee->stack_known = true;
            out.callee->stack.clear();
            // After it returns, only the bytes this function pushed are as they were.
            State back = s;
            for (int reg = 0; reg < 3; ++reg) {
                back.r[reg] = fresh(back, Value{}, id_of(pc, reg));
            }
            unknown_flags(back);
            back.d = Tri::unknown;
            out.edges.emplace_back(next, std::move(back));
            break;
        }
        case Flow::jump_indirect:
            if (insn.operand >= Program::rom_start) {
                // A pointer in ROM. The NMOS 6502 doesn't carry into the high byte.
                const auto high = static_cast<std::uint16_t>((insn.operand & 0xff00) | ((insn.operand + 1) & 0xff));
                out.transfer = Transfer::jump;
                out.target = static_cast<std::uint16_t>(rom(insn.operand) | (rom(high) << 8));
                out.edges.emplace_back(out.target, s);
            } else {
                out.transfer = Transfer::computed_jump;
                out.findings.push_back({Issue::computed_jump, pc, disassemble(insn, pc)});
            }
            break;
        case Flow::ret:
            if (!s.stack_known) {
                out.transfer = Transfer::ret;
                out.findings.push_back({Issue::unknown_stack, pc, "RTS: the bytes pushed since entry aren't known"});
            } else if (s.stack.empty()) {
                out.transfer = Transfer::ret;
            } else if (s.stack.size() == 1) {
                out.transfer = Transfer::computed_jump;
                out.findings.push_back({Issue::computed_jump, pc,
                                        "RTS with one byte pushed: to an address made of it and half the return address"});
            } else {
                rts_through_pushes(pc, s, out);
            }
            break;
        case Flow::rti:
            out.transfer = Transfer::rti;
            break;
        case Flow::brk:
            out.transfer = Transfer::brk;
            out.findings.push_back({Issue::brk, pc, "BRK"});
            break;
        case Flow::halt:
            out.transfer = Transfer::halt;
            out.findings.push_back({Issue::halt, pc, disassemble(insn, pc)});
            break;
        }
        return out;
    }

    // Whether code could start here: the instructions until the first
    // transfer of control (at most 16) are documented and in ROM.
    bool plausible_code(std::uint16_t pc) const {
        for (int i = 0; i < 16; ++i) {
            if (pc < Program::rom_start || pc > 0xfff9) {
                return false;
            }
            const Instruction insn = decode(rom(pc), rom(pc + 1u), rom(pc + 2u));
            if (!insn.documented || insn.flow == Flow::brk) {
                return false;
            }
            if (insn.flow != Flow::next && insn.flow != Flow::call) {
                return true;
            }
            pc = insn.next(pc);
        }
        return true;
    }

    const Instruction& decoded(std::uint16_t pc) {
        auto it = program.nodes_.find(pc);
        if (it == program.nodes_.end()) {
            Node node;
            node.insn = decode(rom(pc), rom(pc + 1u), rom(pc + 2u));
            it = program.nodes_.emplace(pc, std::move(node)).first;
        }
        return it->second.insn;
    }

    void reach(std::uint16_t pc, State s) {
        if (pc < Program::rom_start) {
            return;   // reported in the final pass
        }
        int depth = s.stack_known ? static_cast<int>(s.stack.size()) : -1;
        auto& known = depths[pc];
        if (!known.contains(depth) && known.size() >= max_depths) {
            s.stack_known = false;
            s.stack.clear();
            depth = -1;
        }
        known.insert(depth);
        const Key key{pc, depth};
        auto it = states.find(key);
        if (it == states.end()) {
            states.emplace(key, std::move(s));
        } else {
            State joined = join(it->second, s);
            if (++visits[key] > 64) {
                // Widen: give up on the registers here rather than count through bytes.
                joined.r[A] = joined.r[X] = joined.r[Y] = Value{};
                joined.src = {};
                joined.n = joined.z = joined.c = joined.v = Tri::unknown;
            }
            if (joined == it->second) {
                return;
            }
            it->second = std::move(joined);
        }
        if (queued.insert(key).second) {
            work.push_back(key);
        }
    }

    void enter(std::uint16_t entry) {
        program.functions_.insert(entry);
        reach(entry, State{});
    }

    void run() {
        while (!work.empty()) {
            const Key key = work.front();
            work.pop_front();
            queued.erase(key);
            const Instruction insn = decoded(key.first);
            Outcome out = follow(key.first, insn, states.at(key));
            if (out.call && *out.call >= Program::rom_start) {
                program.functions_.insert(*out.call);
                reach(*out.call, std::move(*out.callee));
            }
            for (auto& [target, state] : out.edges) {
                reach(target, std::move(state));
            }
        }
    }

    // Records each node's final edges, and the findings.
    void finish() {
        std::set<std::pair<Issue, std::uint16_t>> seen;
        const auto report = [&](const Finding& f) {
            if (seen.insert({f.issue, f.at}).second) {
                program.findings_.push_back(f);
            }
        };
        for (auto& [pc, node] : program.nodes_) {
            node.next.clear();
            node.table.clear();
            node.never_taken = node.always_taken = true;
            bool returns = false;
            for (const int depth : depths.at(pc)) {
                const Outcome out = follow(pc, node.insn, states.at({pc, depth}));
                // A table in any context makes the node a table; it may also return.
                if (node.table.empty() || out.transfer == Transfer::table) {
                    node.transfer = out.transfer;
                }
                returns = returns || out.transfer == Transfer::ret;
                node.target = out.target;
                node.never_taken = node.never_taken && out.never_taken;
                node.always_taken = node.always_taken && out.always_taken;
                for (const std::uint16_t target : out.table) {
                    if (std::ranges::find(node.table, target) == node.table.end()) {
                        node.table.push_back(target);
                    }
                }
                for (const auto& [target, state] : out.edges) {
                    if (std::ranges::find(node.next, target) == node.next.end()) {
                        node.next.push_back(target);
                    }
                    if (target < Program::rom_start) {
                        report({Issue::outside_rom, pc, std::format("control goes to {:04X}", target)});
                    }
                }
                if (out.call) {
                    program.return_sites_.insert(node.insn.next(pc));
                    if (*out.call < Program::rom_start) {
                        report({Issue::outside_rom, pc, std::format("calls {:04X}", *out.call)});
                    }
                }
                for (const Finding& f : out.findings) {
                    report(f);
                }
            }
            node.returns = returns && node.transfer == Transfer::table;
            if (node.transfer != Transfer::branch) {
                node.never_taken = node.always_taken = false;
            }
            std::ranges::sort(node.table);
        }
        std::set<std::uint16_t> operand_bytes;
        for (const auto& [pc, node] : program.nodes_) {
            for (unsigned i = 1; i < node.insn.length; ++i) {
                operand_bytes.insert(static_cast<std::uint16_t>(pc + i));
            }
        }
        for (const auto& [pc, node] : program.nodes_) {
            if (operand_bytes.contains(pc)) {
                report({Issue::overlap, pc, std::format("{:04X} is also an operand byte", pc)});
            }
        }
        std::ranges::stable_sort(program.findings_, [](const Finding& a, const Finding& b) {
            return std::pair{a.issue, a.at} < std::pair{b.issue, b.at};
        });
    }
};

Program Program::analyze(std::span<const std::uint8_t> memory, const std::vector<std::uint16_t>& seeds) {
    Program program;
    program.memory_ = memory;
    Interpreter interpreter(program);
    for (const std::uint16_t entry : {program.reset_entry(), program.nmi_entry(), program.irq_entry()}) {
        interpreter.enter(entry);
    }
    for (const std::uint16_t entry : seeds) {
        interpreter.enter(entry);
    }
    interpreter.run();
    interpreter.finish();
    return program;
}

const char* issue_name(Issue issue) noexcept {
    switch (issue) {
    case Issue::computed_jump: return "computed jump";
    case Issue::unknown_stack: return "RTS with unknown pushes";
    case Issue::pops_return: return "pulls its caller's return address";
    case Issue::implausible_entry: return "table slot that isn't code";
    case Issue::undocumented: return "undocumented opcode";
    case Issue::brk: return "BRK";
    case Issue::halt: return "KIL";
    case Issue::outside_rom: return "outside ROM";
    case Issue::overlap: return "overlap";
    }
    return "?";
}

} // namespace qilin::m6502
