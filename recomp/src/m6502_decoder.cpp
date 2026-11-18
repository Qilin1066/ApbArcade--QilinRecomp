#include "m6502_decoder.hpp"

#include <array>
#include <cstdio>

namespace qilin::m6502 {

namespace {

struct Entry {
    Op op;
    Mode mode;
};

using enum Op;
using M = Mode;

// The NMOS 6502 opcode matrix, row = high nibble.
constexpr std::array<Entry, 256> table{{
    // 0x00
    {brk, M::imp}, {ora, M::idx}, {kil, M::non}, {slo, M::idx}, {nop, M::zpg}, {ora, M::zpg}, {asl, M::zpg}, {slo, M::zpg},
    {php, M::imp}, {ora, M::imm}, {asl, M::acc}, {anc, M::imm}, {nop, M::aba}, {ora, M::aba}, {asl, M::aba}, {slo, M::aba},
    // 0x10
    {bpl, M::rel}, {ora, M::idy}, {kil, M::non}, {slo, M::idy}, {nop, M::zpx}, {ora, M::zpx}, {asl, M::zpx}, {slo, M::zpx},
    {clc, M::imp}, {ora, M::aby}, {nop, M::imp}, {slo, M::aby}, {nop, M::abx}, {ora, M::abx}, {asl, M::abx}, {slo, M::abx},
    // 0x20
    {jsr, M::adr}, {and_, M::idx}, {kil, M::non}, {rla, M::idx}, {bit, M::zpg}, {and_, M::zpg}, {rol, M::zpg}, {rla, M::zpg},
    {plp, M::imp}, {and_, M::imm}, {rol, M::acc}, {anc, M::imm}, {bit, M::aba}, {and_, M::aba}, {rol, M::aba}, {rla, M::aba},
    // 0x30
    {bmi, M::rel}, {and_, M::idy}, {kil, M::non}, {rla, M::idy}, {nop, M::zpx}, {and_, M::zpx}, {rol, M::zpx}, {rla, M::zpx},
    {sec, M::imp}, {and_, M::aby}, {nop, M::imp}, {rla, M::aby}, {nop, M::abx}, {and_, M::abx}, {rol, M::abx}, {rla, M::abx},
    // 0x40
    {rti, M::imp}, {eor, M::idx}, {kil, M::non}, {sre, M::idx}, {nop, M::zpg}, {eor, M::zpg}, {lsr, M::zpg}, {sre, M::zpg},
    {pha, M::imp}, {eor, M::imm}, {lsr, M::acc}, {asr, M::imm}, {jmp, M::adr}, {eor, M::aba}, {lsr, M::aba}, {sre, M::aba},
    // 0x50
    {bvc, M::rel}, {eor, M::idy}, {kil, M::non}, {sre, M::idy}, {nop, M::zpx}, {eor, M::zpx}, {lsr, M::zpx}, {sre, M::zpx},
    {cli, M::imp}, {eor, M::aby}, {nop, M::imp}, {sre, M::aby}, {nop, M::abx}, {eor, M::abx}, {lsr, M::abx}, {sre, M::abx},
    // 0x60
    {rts, M::imp}, {adc, M::idx}, {kil, M::non}, {rra, M::idx}, {nop, M::zpg}, {adc, M::zpg}, {ror, M::zpg}, {rra, M::zpg},
    {pla, M::imp}, {adc, M::imm}, {ror, M::acc}, {arr, M::imm}, {jmp, M::ind}, {adc, M::aba}, {ror, M::aba}, {rra, M::aba},
    // 0x70
    {bvs, M::rel}, {adc, M::idy}, {kil, M::non}, {rra, M::idy}, {nop, M::zpx}, {adc, M::zpx}, {ror, M::zpx}, {rra, M::zpx},
    {sei, M::imp}, {adc, M::aby}, {nop, M::imp}, {rra, M::aby}, {nop, M::abx}, {adc, M::abx}, {ror, M::abx}, {rra, M::abx},
    // 0x80
    {nop, M::imm}, {sta, M::idx}, {nop, M::imm}, {sax, M::idx}, {sty, M::zpg}, {sta, M::zpg}, {stx, M::zpg}, {sax, M::zpg},
    {dey, M::imp}, {nop, M::imm}, {txa, M::imp}, {ane, M::imm}, {sty, M::aba}, {sta, M::aba}, {stx, M::aba}, {sax, M::aba},
    // 0x90
    {bcc, M::rel}, {sta, M::idy}, {kil, M::non}, {sha, M::idy}, {sty, M::zpx}, {sta, M::zpx}, {stx, M::zpy}, {sax, M::zpy},
    {tya, M::imp}, {sta, M::aby}, {txs, M::imp}, {shs, M::aby}, {shy, M::abx}, {sta, M::abx}, {shx, M::aby}, {sha, M::aby},
    // 0xA0
    {ldy, M::imm}, {lda, M::idx}, {ldx, M::imm}, {lax, M::idx}, {ldy, M::zpg}, {lda, M::zpg}, {ldx, M::zpg}, {lax, M::zpg},
    {tay, M::imp}, {lda, M::imm}, {tax, M::imp}, {lxa, M::imm}, {ldy, M::aba}, {lda, M::aba}, {ldx, M::aba}, {lax, M::aba},
    // 0xB0
    {bcs, M::rel}, {lda, M::idy}, {kil, M::non}, {lax, M::idy}, {ldy, M::zpx}, {lda, M::zpx}, {ldx, M::zpy}, {lax, M::zpy},
    {clv, M::imp}, {lda, M::aby}, {tsx, M::imp}, {las, M::aby}, {ldy, M::abx}, {lda, M::abx}, {ldx, M::aby}, {lax, M::aby},
    // 0xC0
    {cpy, M::imm}, {cmp, M::idx}, {nop, M::imm}, {dcp, M::idx}, {cpy, M::zpg}, {cmp, M::zpg}, {dec, M::zpg}, {dcp, M::zpg},
    {iny, M::imp}, {cmp, M::imm}, {dex, M::imp}, {sbx, M::imm}, {cpy, M::aba}, {cmp, M::aba}, {dec, M::aba}, {dcp, M::aba},
    // 0xD0
    {bne, M::rel}, {cmp, M::idy}, {kil, M::non}, {dcp, M::idy}, {nop, M::zpx}, {cmp, M::zpx}, {dec, M::zpx}, {dcp, M::zpx},
    {cld, M::imp}, {cmp, M::aby}, {nop, M::imp}, {dcp, M::aby}, {nop, M::abx}, {cmp, M::abx}, {dec, M::abx}, {dcp, M::abx},
    // 0xE0
    {cpx, M::imm}, {sbc, M::idx}, {nop, M::imm}, {isb, M::idx}, {cpx, M::zpg}, {sbc, M::zpg}, {inc, M::zpg}, {isb, M::zpg},
    {inx, M::imp}, {sbc, M::imm}, {nop, M::imp}, {sbc, M::imm}, {cpx, M::aba}, {sbc, M::aba}, {inc, M::aba}, {isb, M::aba},
    // 0xF0
    {beq, M::rel}, {sbc, M::idy}, {kil, M::non}, {isb, M::idy}, {nop, M::zpx}, {sbc, M::zpx}, {inc, M::zpx}, {isb, M::zpx},
    {sed, M::imp}, {sbc, M::aby}, {nop, M::imp}, {isb, M::aby}, {nop, M::abx}, {sbc, M::abx}, {inc, M::abx}, {isb, M::abx},
}};

constexpr bool is_branch(Op op) {
    switch (op) {
    case bcc: case bcs: case beq: case bmi: case bne: case bpl: case bvc: case bvs:
        return true;
    default:
        return false;
    }
}

constexpr Access access_of(Op op, Mode mode) {
    if (mode == M::imp || mode == M::acc || mode == M::non || mode == M::rel || mode == M::adr ||
        mode == M::ind) {
        return Access::none;
    }
    switch (op) {
    case sta: case stx: case sty: case sax: case sha: case shs: case shx: case shy:
        return Access::write;
    case asl: case lsr: case rol: case ror: case inc: case dec:
    case slo: case rla: case sre: case rra: case dcp: case isb:
        return Access::rmw;
    default:
        return Access::read;   // loads, arithmetic, compares, BIT, the reading NOPs
    }
}

constexpr Flow flow_of(Op op, Mode mode) {
    if (is_branch(op)) {
        return Flow::branch;
    }
    switch (op) {
    case jmp: return mode == M::ind ? Flow::jump_indirect : Flow::jump;
    case jsr: return Flow::call;
    case rts: return Flow::ret;
    case rti: return Flow::rti;
    case brk: return Flow::brk;
    case kil: return Flow::halt;
    default: return Flow::next;
    }
}

constexpr std::uint8_t length_of(Mode mode) {
    switch (mode) {
    case M::non: case M::imp: case M::acc:
        return 1;
    case M::imm: case M::zpg: case M::zpx: case M::zpy: case M::idx: case M::idy: case M::rel:
        return 2;
    case M::aba: case M::abx: case M::aby: case M::ind: case M::adr:
        return 3;
    }
    return 1;
}

constexpr std::uint8_t cycles_of(Op op, Mode mode, Access access) {
    switch (op) {
    case brk: return 7;
    case jsr: case rts: case rti: return 6;
    case jmp: return mode == M::ind ? 5 : 3;
    case pha: case php: return 3;
    case pla: case plp: return 4;
    case kil: return 0;
    default: break;
    }
    if (mode == M::rel || mode == M::imp || mode == M::acc || mode == M::imm) {
        return 2;
    }
    // Memory operands: the address, then the access.
    const bool rmw = access == Access::rmw;
    const bool write = access == Access::write;
    switch (mode) {
    case M::zpg: return rmw ? 5 : 3;
    case M::zpx: case M::zpy: return rmw ? 6 : 4;
    case M::aba: return rmw ? 6 : 4;
    case M::abx: case M::aby: return rmw ? 7 : write ? 5 : 4;
    case M::idx: return rmw ? 8 : 6;
    case M::idy: return rmw ? 8 : write ? 6 : 5;
    default: return 0;
    }
}

constexpr bool documented_op(Op op) { return op <= tya; }

} // namespace

Instruction decode(std::uint8_t opcode, std::uint8_t b1, std::uint8_t b2) noexcept {
    const Entry& e = table[opcode];
    Instruction insn;
    insn.opcode = opcode;
    insn.op = e.op;
    insn.mode = e.mode;
    insn.access = access_of(e.op, e.mode);
    insn.flow = flow_of(e.op, e.mode);
    insn.length = length_of(e.mode);
    insn.cycles = cycles_of(e.op, e.mode, insn.access);
    insn.page_penalty = insn.access == Access::read &&
                        (e.mode == M::abx || e.mode == M::aby || e.mode == M::idy);
    // 0xEA is the NOP; the other NOP encodings, and SBC at 0xEB, are not documented.
    insn.documented = documented_op(e.op) && opcode != 0xeb && (e.op != nop || opcode == 0xea);
    if (insn.length == 2) {
        insn.operand = b1;
    } else if (insn.length == 3) {
        insn.operand = static_cast<std::uint16_t>(b1 | (b2 << 8));
    }
    return insn;
}

bool branch_taken(Op op, std::uint8_t p) noexcept {
    switch (op) {
    case bpl: return !(p & flag_n);
    case bmi: return (p & flag_n) != 0;
    case bvc: return !(p & flag_v);
    case bvs: return (p & flag_v) != 0;
    case bcc: return !(p & flag_c);
    case bcs: return (p & flag_c) != 0;
    case bne: return !(p & flag_z);
    case beq: return (p & flag_z) != 0;
    default: return false;
    }
}

const char* mnemonic(Op op) noexcept {
    static constexpr const char* names[] = {
        "adc", "and", "asl", "bcc", "bcs", "beq", "bit", "bmi", "bne", "bpl", "brk", "bvc", "bvs",
        "clc", "cld", "cli", "clv", "cmp", "cpx", "cpy", "dec", "dex", "dey", "eor", "inc", "inx",
        "iny", "jmp", "jsr", "lda", "ldx", "ldy", "lsr", "nop", "ora", "pha", "php", "pla", "plp",
        "rol", "ror", "rti", "rts", "sbc", "sec", "sed", "sei", "sta", "stx", "sty", "tax", "tay",
        "tsx", "txa", "txs", "tya",
        "anc", "ane", "arr", "asr", "dcp", "isb", "kil", "las", "lax", "lxa", "rla", "rra", "sax",
        "sbx", "sha", "shs", "shx", "shy", "slo", "sre",
    };
    static_assert(std::size(names) == static_cast<std::size_t>(sre) + 1);
    return names[static_cast<std::size_t>(op)];
}

std::string disassemble(const Instruction& insn, std::uint16_t pc) {
    char text[32];
    const char* name = mnemonic(insn.op);
    const unsigned v = insn.operand;
    switch (insn.mode) {
    case M::non: case M::imp: std::snprintf(text, sizeof text, "%s", name); break;
    case M::acc: std::snprintf(text, sizeof text, "%s a", name); break;
    case M::imm: std::snprintf(text, sizeof text, "%s #$%02x", name, v); break;
    case M::zpg: std::snprintf(text, sizeof text, "%s $%02x", name, v); break;
    case M::zpx: std::snprintf(text, sizeof text, "%s $%02x, x", name, v); break;
    case M::zpy: std::snprintf(text, sizeof text, "%s $%02x, y", name, v); break;
    case M::aba: case M::adr: std::snprintf(text, sizeof text, "%s $%04x", name, v); break;
    case M::abx: std::snprintf(text, sizeof text, "%s $%04x, x", name, v); break;
    case M::aby: std::snprintf(text, sizeof text, "%s $%04x, y", name, v); break;
    case M::idx: std::snprintf(text, sizeof text, "%s ($%02x, x)", name, v); break;
    case M::idy: std::snprintf(text, sizeof text, "%s ($%02x), y", name, v); break;
    case M::ind: std::snprintf(text, sizeof text, "%s ($%04x)", name, v); break;
    case M::rel: std::snprintf(text, sizeof text, "%s $%04x", name, insn.branch_target(pc)); break;
    }
    return text;
}

} // namespace qilin::m6502
