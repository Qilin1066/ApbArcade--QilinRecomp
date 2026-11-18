#include "t11_decoder.hpp"

#include <array>
#include <cstdio>

namespace qilin::t11 {
namespace {

// MAME's cycle costs for an operand, by addressing mode (R7 forms cost the
// same as the general ones). A destination the instruction writes costs more
// than one it only reads.
constexpr std::array<std::uint8_t, 8> read_cost{0, 6, 6, 12, 9, 15, 15, 21};
constexpr std::array<std::uint8_t, 8> write_cost{0, 9, 9, 15, 12, 18, 18, 24};
constexpr std::array<std::uint8_t, 8> jump_cost{0, 15, 18, 18, 18, 21, 21, 27};
constexpr std::uint8_t trap_cycles = 48;

constexpr Operand operand(unsigned field) noexcept {
    return {static_cast<std::uint8_t>((field >> 3) & 7), static_cast<std::uint8_t>(field & 7), 0};
}

struct Words {
    std::uint16_t w1, w2;
    unsigned used = 0;
    std::uint16_t next() noexcept { return used++ == 0 ? w1 : w2; }
};

void take_word(Operand& op, Words& words) noexcept {
    if (op.has_word()) {
        op.word = words.next();
    }
}

} // namespace

Instruction decode(std::uint16_t w0, std::uint16_t w1, std::uint16_t w2) noexcept {
    Instruction insn;
    insn.opcode = w0;
    insn.cycles = trap_cycles;          // reserved words trap
    Words words{w1, w2};
    const unsigned lo = w0 & 077;
    const unsigned hi = (w0 >> 6) & 077;
    const bool byte_group = (w0 & 0100000) != 0;

    const auto branch = [&](Op op) {
        insn.op = op;
        insn.legal = true;
        insn.displacement = static_cast<std::int16_t>(2 * static_cast<std::int8_t>(w0 & 0377));
        insn.cycles = 12;
    };
    const auto one = [&](Op op, bool reads_only = false) {
        insn.op = op;
        insn.legal = true;
        insn.operands = 1;
        insn.dst = operand(lo);
        take_word(insn.dst, words);
        insn.cycles = static_cast<std::uint8_t>(12 + (reads_only ? read_cost : write_cost)[insn.dst.mode]);
    };
    const auto two = [&](Op op, bool reads_only) {
        insn.op = op;
        insn.legal = true;
        insn.operands = 2;
        insn.src = operand(hi);
        insn.dst = operand(lo);
        take_word(insn.src, words);
        take_word(insn.dst, words);
        insn.cycles = static_cast<std::uint8_t>(12 + read_cost[insn.src.mode] +
                                                (reads_only ? read_cost : write_cost)[insn.dst.mode]);
    };
    const auto jump = [&](Op op, unsigned extra) {
        insn.op = op;
        insn.operands = 1;
        insn.dst = operand(lo);
        take_word(insn.dst, words);
        insn.legal = insn.dst.mode != reg;
        insn.cycles = insn.legal ? static_cast<std::uint8_t>(jump_cost[insn.dst.mode] + extra) : trap_cycles;
    };
    // EIS and MARK: decoded for the listing, executed as a reserved-instruction trap.
    const auto not_t11 = [&](Op op, bool with_register) {
        insn.op = op;
        insn.operands = 1;
        insn.dst = operand(lo);
        take_word(insn.dst, words);
        insn.reg = with_register ? static_cast<std::uint8_t>(hi & 7) : 0;
    };
    const auto one_operand = [&](unsigned group) {
        insn.byte = byte_group;
        switch (group) {
        case 005000: one(Op::clr); break;
        case 005100: one(Op::com); break;
        case 005200: one(Op::inc); break;
        case 005300: one(Op::dec); break;
        case 005400: one(Op::neg); break;
        case 005500: one(Op::adc); break;
        case 005600: one(Op::sbc); break;
        case 005700: one(Op::tst, true); break;
        case 006000: one(Op::ror); break;
        case 006100: one(Op::rol); break;
        case 006200: one(Op::asr); break;
        case 006300: one(Op::asl); break;
        case 006400:
            if (byte_group) {
                one(Op::mtps, true);
                insn.cycles = static_cast<std::uint8_t>(24 + read_cost[insn.dst.mode]);
            } else {
                insn.byte = false;
                not_t11(Op::mark, false);
            }
            break;
        case 006700: one(byte_group ? Op::mfps : Op::sxt); break;
        default: insn.byte = false; break;  // 0065xx, 0066xx: reserved
        }
    };

    const unsigned top = (w0 >> 12) & 7;
    if (top == 0 && !byte_group) {
        if (w0 < 0000400) {
            switch (w0 & 0177700) {
            case 0000000: {
                static constexpr std::array<Op, 8> ops{Op::halt, Op::wait, Op::rti, Op::bpt,
                                                       Op::iot, Op::reset, Op::rtt, Op::mfpt};
                // MAME charges nothing for WAIT (it ends the timeslice) or MFPT.
                static constexpr std::array<std::uint8_t, 8> cycles{48, 0, 24, 48, 48, 110, 33, 0};
                if (lo < 8) {
                    insn.op = ops[lo];
                    insn.legal = true;
                    insn.cycles = cycles[lo];
                }
                break;
            }
            case 0000100:
                jump(Op::jmp, 0);
                break;
            case 0000200:
                if ((lo & 070) == 0) {
                    insn.op = Op::rts;
                    insn.legal = true;
                    insn.reg = static_cast<std::uint8_t>(lo & 7);
                    insn.cycles = 21;
                } else if (lo >= 040) {
                    insn.op = (lo & 020) ? Op::scc : Op::ccc;
                    insn.legal = true;
                    insn.reg = static_cast<std::uint8_t>(lo & 017);
                    insn.cycles = 18;
                }
                break;
            default:
                one(Op::swab);
                break;
            }
        } else if (w0 < 0004000) {
            static constexpr std::array<Op, 8> ops{Op::br, Op::br, Op::bne, Op::beq,
                                                   Op::bge, Op::blt, Op::bgt, Op::ble};
            branch(ops[(w0 >> 8) & 7]);
        } else if (w0 < 0005000) {
            jump(Op::jsr, 12);
            insn.reg = static_cast<std::uint8_t>(hi & 7);
        } else if (w0 < 0007000) {
            one_operand(w0 & 0177700);
        }
    } else if (top == 0) {
        if (w0 < 0104000) {
            static constexpr std::array<Op, 8> ops{Op::bpl, Op::bmi, Op::bhi, Op::blos,
                                                   Op::bvc, Op::bvs, Op::bcc, Op::bcs};
            branch(ops[(w0 >> 8) & 7]);
        } else if (w0 < 0105000) {
            insn.op = (w0 & 0400) ? Op::trap : Op::emt;
            insn.legal = true;
            insn.reg = static_cast<std::uint8_t>(w0 & 0377);
        } else if (w0 < 0107000) {
            one_operand(w0 & 0077700);
        }
    } else if (top == 7) {
        if (!byte_group) {
            switch (w0 & 0177000) {
            case 0070000: not_t11(Op::mul, true); break;
            case 0071000: not_t11(Op::div, true); break;
            case 0072000: not_t11(Op::ash, true); break;
            case 0073000: not_t11(Op::ashc, true); break;
            case 0074000:
                one(Op::xor_);
                insn.reg = static_cast<std::uint8_t>(hi & 7);
                break;
            case 0077000:
                insn.op = Op::sob;
                insn.legal = true;
                insn.reg = static_cast<std::uint8_t>(hi & 7);
                insn.displacement = static_cast<std::int16_t>(-2 * static_cast<int>(lo));
                insn.cycles = 18;
                break;
            default:   // 075xxx, 076xxx: floating point and commercial sets
                break;
            }
        }
    } else if (top == 6) {
        two(byte_group ? Op::sub : Op::add, false);
    } else {
        static constexpr std::array<Op, 6> ops{Op::reserved, Op::mov, Op::cmp, Op::bit, Op::bic, Op::bis};
        insn.byte = byte_group;
        two(ops[top], top == 2 || top == 3);
    }
    insn.words = static_cast<std::uint8_t>(1 + words.used);
    return insn;
}

int trap_vector(const Instruction& insn) noexcept {
    if (!insn.legal) {
        return (insn.op == Op::jmp || insn.op == Op::jsr) ? 004 : 010;
    }
    switch (insn.op) {
    case Op::bpt: return 014;
    case Op::iot: return 020;
    case Op::emt: return 030;
    case Op::trap: return 034;
    default: return -1;
    }
}

const char* mnemonic(Op op) noexcept {
    switch (op) {
    case Op::halt: return "HALT";
    case Op::wait: return "WAIT";
    case Op::rti: return "RTI";
    case Op::bpt: return "BPT";
    case Op::iot: return "IOT";
    case Op::reset: return "RESET";
    case Op::rtt: return "RTT";
    case Op::mfpt: return "MFPT";
    case Op::jmp: return "JMP";
    case Op::rts: return "RTS";
    case Op::ccc: return "CCC";
    case Op::scc: return "SCC";
    case Op::jsr: return "JSR";
    case Op::sob: return "SOB";
    case Op::emt: return "EMT";
    case Op::trap: return "TRAP";
    case Op::br: return "BR";
    case Op::bne: return "BNE";
    case Op::beq: return "BEQ";
    case Op::bge: return "BGE";
    case Op::blt: return "BLT";
    case Op::bgt: return "BGT";
    case Op::ble: return "BLE";
    case Op::bpl: return "BPL";
    case Op::bmi: return "BMI";
    case Op::bhi: return "BHI";
    case Op::blos: return "BLOS";
    case Op::bvc: return "BVC";
    case Op::bvs: return "BVS";
    case Op::bcc: return "BCC";
    case Op::bcs: return "BCS";
    case Op::swab: return "SWAB";
    case Op::clr: return "CLR";
    case Op::com: return "COM";
    case Op::inc: return "INC";
    case Op::dec: return "DEC";
    case Op::neg: return "NEG";
    case Op::adc: return "ADC";
    case Op::sbc: return "SBC";
    case Op::tst: return "TST";
    case Op::ror: return "ROR";
    case Op::rol: return "ROL";
    case Op::asr: return "ASR";
    case Op::asl: return "ASL";
    case Op::sxt: return "SXT";
    case Op::mtps: return "MTPS";
    case Op::mfps: return "MFPS";
    case Op::mov: return "MOV";
    case Op::cmp: return "CMP";
    case Op::bit: return "BIT";
    case Op::bic: return "BIC";
    case Op::bis: return "BIS";
    case Op::add: return "ADD";
    case Op::sub: return "SUB";
    case Op::xor_: return "XOR";
    case Op::mark: return "MARK";
    case Op::mul: return "MUL";
    case Op::div: return "DIV";
    case Op::ash: return "ASH";
    case Op::ashc: return "ASHC";
    case Op::reserved: return ".WORD";
    }
    return "?";
}

namespace {

constexpr std::array<const char*, 8> register_names{"R0", "R1", "R2", "R3", "R4", "R5", "SP", "PC"};

template <typename... Args>
std::string format(const char* pattern, Args... args) {
    char buffer[64];
    const int length = std::snprintf(buffer, sizeof buffer, pattern, args...);
    return {buffer, static_cast<std::size_t>(length > 0 ? length : 0)};
}

// An operand as MAME's MakeEA prints it. 'pc' is the address after the
// operand's extension word, which relative modes are based on.
std::string operand_text(const Operand& op, bool byte, unsigned pc) {
    const auto name = register_names[op.reg];
    const unsigned word = op.word;
    const bool negative = (word & 0x8000) != 0;
    const unsigned magnitude = negative ? (0x10000 - word) : word;
    switch (op.mode) {
    case reg: return name;
    case reg_deferred: return format("(%s)", name);
    case autoinc:
        return op.reg == 7 ? format("#%0*o", byte ? 3 : 6, word & (byte ? 0377u : 0177777u)) : format("(%s)+", name);
    case autoinc_deferred:
        return op.reg == 7 ? format("@#%06o", word) : format("@(%s)+", name);
    case autodec: return format("-(%s)", name);
    case autodec_deferred: return format("@-(%s)", name);
    case index:
        return op.reg == 7 ? format("%06o", (word + pc) & 0177777u)
                           : format("%s%o(%s)", negative ? "-" : "", magnitude, name);
    default:
        return op.reg == 7 ? format("@%06o", (word + pc) & 0177777u)
                           : format("@%s%o(%s)", negative ? "-" : "", magnitude, name);
    }
}

std::string padded(const char* name, bool byte) {
    std::string text = name;
    if (byte) {
        text += 'B';
    }
    text.resize(6, ' ');
    return text;
}

} // namespace

std::string disassemble(const Instruction& insn, std::uint16_t pc) {
    unsigned next = pc + 2u;
    const auto ea = [&](const Operand& op, bool byte) {
        if (op.has_word()) {
            next += 2;
        }
        return operand_text(op, byte, next);
    };
    // MTPS and MFPS move a byte, but have no B in their names.
    const auto name = [&] {
        return padded(mnemonic(insn.op), insn.byte && insn.op != Op::mtps && insn.op != Op::mfps);
    };

    switch (insn.op) {
    case Op::halt: case Op::wait: case Op::rti: case Op::bpt:
    case Op::iot: case Op::reset: case Op::rtt: case Op::mfpt:
        return mnemonic(insn.op);
    case Op::reserved:
        return format(".WORD %06o", insn.opcode);
    case Op::rts:
        return insn.reg == 7 ? std::string("RETURN") : "RTS   " + std::string(register_names[insn.reg]);
    case Op::ccc:
    case Op::scc: {
        const bool set = insn.op == Op::scc;
        switch (insn.reg) {
        case 000: return "NOP";
        case 017: return set ? "SCC" : "CCC";
        case 001: return set ? "SEC" : "CLC";
        case 002: return set ? "SEV" : "CLV";
        case 004: return set ? "SEZ" : "CLZ";
        case 010: return set ? "SEN" : "CLN";
        default: return format("%s   #%02o", set ? "Scc" : "Ccc", insn.reg);
        }
    }
    case Op::br: case Op::bne: case Op::beq: case Op::bge: case Op::blt: case Op::bgt: case Op::ble:
    case Op::bpl: case Op::bmi: case Op::bhi: case Op::blos: case Op::bvc: case Op::bvs: case Op::bcc: case Op::bcs:
        return name() + format("%06o", insn.branch_target(pc));
    case Op::sob:
        return name() + format("%s,%06o", register_names[insn.reg], insn.branch_target(pc));
    case Op::emt:
    case Op::trap:
        return name() + format("%03o", insn.reg);
    case Op::jsr: {
        const auto target = ea(insn.dst, false);
        return insn.reg == 7 ? "CALL  " + target : name() + register_names[insn.reg] + "," + target;
    }
    case Op::xor_:
        return name() + register_names[insn.reg] + "," + ea(insn.dst, false);
    case Op::mul: case Op::div: case Op::ash: case Op::ashc:
        return name() + ea(insn.dst, false) + "," + register_names[insn.reg];
    default:
        break;
    }
    if (insn.operands == 2) {
        const auto source = ea(insn.src, insn.byte);
        return name() + source + "," + ea(insn.dst, insn.byte);
    }
    return name() + ea(insn.dst, insn.byte);
}

} // namespace qilin::t11
