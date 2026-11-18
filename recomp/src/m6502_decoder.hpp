#pragma once
// MOS 6502 (NMOS) instruction decoder, for System 2's sound CPU.
//
// Decodes all 256 opcodes as the NMOS 6502 executes them, the undocumented
// ones included (under MAME's names). Instruction::documented tells the two
// apart: APB's sound program should use only documented opcodes, so an
// undocumented one met during analysis usually means the analysis has walked
// into data.
//
// Cycle counts are MAME's (src/devices/cpu/m6502/om6502.lst), which is this
// project's reference. tools/check/m6502_decoder_check.cpp verifies the
// length, cycle count, page-crossing penalty, control flow and MAME-format
// disassembly of every opcode against MAME's own core and disassembler.

#include <cstdint>
#include <string>

namespace qilin::m6502 {

enum class Op : std::uint8_t {
    // Documented
    adc, and_, asl, bcc, bcs, beq, bit, bmi, bne, bpl, brk, bvc, bvs, clc, cld, cli, clv,
    cmp, cpx, cpy, dec, dex, dey, eor, inc, inx, iny, jmp, jsr, lda, ldx, ldy, lsr, nop,
    ora, pha, php, pla, plp, rol, ror, rti, rts, sbc, sec, sed, sei, sta, stx, sty,
    tax, tay, tsx, txa, txs, tya,
    // Undocumented (MAME's names)
    anc, ane, arr, asr, dcp, isb, kil, las, lax, lxa, rla, rra, sax, sbx, sha, shs, shx,
    shy, slo, sre,
};

enum class Mode : std::uint8_t {
    non,  // KIL: no operand
    imp,  // implied
    acc,  // accumulator
    imm,  // #nn
    zpg,  // nn
    zpx,  // nn,X
    zpy,  // nn,Y
    aba,  // nnnn
    abx,  // nnnn,X
    aby,  // nnnn,Y
    idx,  // (nn,X)
    idy,  // (nn),Y
    ind,  // (nnnn), JMP only
    rel,  // branch displacement
    adr,  // nnnn as a code address: JMP, JSR
};

// What an instruction does with its memory operand.
enum class Access : std::uint8_t { none, read, write, rmw };

enum class Flow : std::uint8_t {
    next,           // falls through
    branch,         // conditional, relative
    jump,           // JMP nnnn
    jump_indirect,  // JMP (nnnn)
    call,           // JSR nnnn
    ret,            // RTS
    rti,
    brk,            // pushes pc+2 and P|B, then through 0xFFFE
    halt,           // KIL: the CPU stops until reset
};

struct Instruction {
    std::uint8_t opcode = 0;
    Op op = Op::kil;
    Mode mode = Mode::non;
    Access access = Access::none;
    Flow flow = Flow::halt;
    bool documented = false;
    // MAME's length: BRK counts as 1 although it skips the byte after it.
    std::uint8_t length = 1;
    // Cycles, the opcode fetch included. A read through abx, aby or idy that
    // crosses a page takes one more (page_penalty); a taken branch takes one
    // more, and one more again if its target is in another page. KIL: 0.
    std::uint8_t cycles = 0;
    bool page_penalty = false;
    std::uint16_t operand = 0;  // the byte or little-endian word after the opcode

    [[nodiscard]] constexpr std::uint16_t next(std::uint16_t pc) const noexcept {
        return static_cast<std::uint16_t>(pc + length);
    }
    [[nodiscard]] constexpr std::uint16_t branch_target(std::uint16_t pc) const noexcept {
        return static_cast<std::uint16_t>(pc + 2 + static_cast<std::int8_t>(operand & 0xff));
    }
};

// Cycles the 6502 takes to answer an IRQ or NMI, and to come out of reset
// (until the first opcode has been fetched), in MAME.
inline constexpr int interrupt_cycles = 7;
inline constexpr int reset_cycles = 8;

inline constexpr std::uint16_t nmi_vector = 0xfffa;
inline constexpr std::uint16_t reset_vector = 0xfffc;
inline constexpr std::uint16_t irq_vector = 0xfffe;

// P register bits.
inline constexpr std::uint8_t flag_c = 0x01, flag_z = 0x02, flag_i = 0x04, flag_d = 0x08,
                              flag_b = 0x10, flag_v = 0x40, flag_n = 0x80;

// Decodes the opcode, with the two bytes after it (used only when the
// instruction has an operand).
[[nodiscard]] Instruction decode(std::uint8_t opcode, std::uint8_t b1 = 0, std::uint8_t b2 = 0) noexcept;

// Whether a branch is taken with status register p.
[[nodiscard]] bool branch_taken(Op op, std::uint8_t p) noexcept;

// MAME-format disassembly, for an instruction at 'pc'.
[[nodiscard]] std::string disassemble(const Instruction& insn, std::uint16_t pc);

[[nodiscard]] const char* mnemonic(Op op) noexcept;

// MAME's name for an addressing mode ("zpg", "abx" ...): with the mnemonic,
// the name of the opcode's microcode in om6502.lst ("lda_abx").
[[nodiscard]] const char* mode_name(Mode mode) noexcept;

} // namespace qilin::m6502
