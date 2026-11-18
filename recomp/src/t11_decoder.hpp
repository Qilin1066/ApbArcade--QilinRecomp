#pragma once
// DEC T-11 instruction decoder.
//
// Decodes every 16-bit word, as the T-11 in Atari System 2 does: the PDP-11
// base set plus SOB, SXT, XOR, RTT, MFPT, MTPS and MFPS. EIS (MUL, DIV, ASH,
// ASHC) and MARK decode with legal = false: on the T-11 they trap through
// vector 010, as do the unassigned words. JMP and JSR with a register
// destination trap through vector 004.
//
// Cycle counts are MAME's (src/devices/cpu/t11/t11ops.hxx), which is this
// project's reference. tools/check/t11_decoder_check.cpp verifies the length,
// the cycle count and MAME-format disassembly of every opcode against MAME's
// own core and disassembler.

#include <cstdint>
#include <string>

namespace qilin::t11 {

enum class Op : std::uint8_t {
    // No operands
    halt, wait, rti, bpt, iot, reset, rtt, mfpt,
    // Program control
    jmp, rts, ccc, scc, jsr, sob, emt, trap,
    br, bne, beq, bge, blt, bgt, ble, bpl, bmi, bhi, blos, bvc, bvs, bcc, bcs,
    // One operand (byte forms have Instruction::byte set)
    swab, clr, com, inc, dec, neg, adc, sbc, tst, ror, rol, asr, asl, sxt, mtps, mfps,
    // Two operands
    mov, cmp, bit, bic, bis, add, sub, xor_,
    // Not on the T-11: they decode, but trap through vector 010 (legal = false)
    mark, mul, div, ash, ashc,
    // Reserved words: trap through vector 010
    reserved,
};

// Addressing modes 0-7, as in the PDP-11 operand field.
enum Mode : std::uint8_t {
    reg = 0,             // Rn
    reg_deferred = 1,    // (Rn)
    autoinc = 2,         // (Rn)+, #n with R7
    autoinc_deferred = 3,// @(Rn)+, @#n with R7
    autodec = 4,         // -(Rn)
    autodec_deferred = 5,// @-(Rn)
    index = 6,           // X(Rn), relative with R7
    index_deferred = 7,  // @X(Rn), relative deferred with R7
};

struct Operand {
    std::uint8_t mode = 0;
    std::uint8_t reg = 0;
    std::uint16_t word = 0;   // the extension word, when has_word()

    [[nodiscard]] constexpr bool has_word() const noexcept {
        return mode >= index || (reg == 7 && (mode == autoinc || mode == autoinc_deferred));
    }
    [[nodiscard]] constexpr bool immediate() const noexcept { return reg == 7 && mode == autoinc; }
    [[nodiscard]] constexpr bool absolute() const noexcept { return reg == 7 && mode == autoinc_deferred; }
    [[nodiscard]] constexpr bool relative() const noexcept { return reg == 7 && mode == index; }
    [[nodiscard]] constexpr bool relative_deferred() const noexcept { return reg == 7 && mode == index_deferred; }
};

struct Instruction {
    std::uint16_t opcode = 0;
    Op op = Op::reserved;
    bool byte = false;          // byte form (MOVB, CLRB ...)
    bool legal = false;         // executes on the T-11 (false: traps)
    std::uint8_t words = 1;     // 1 to 3, opcode included
    std::uint8_t cycles = 0;    // MAME's count; a trap's includes the trap
    std::uint8_t reg = 0;       // JSR/XOR/SOB register, RTS register, CCC/SCC mask, EMT/TRAP code
    std::int16_t displacement = 0; // branch: byte offset from the next instruction; SOB: negative
    Operand src{};
    Operand dst{};
    std::uint8_t operands = 0;  // how many of src, dst are used (one-operand forms use dst)

    // Address after the opcode and its extension words.
    [[nodiscard]] constexpr std::uint16_t next(std::uint16_t pc) const noexcept {
        return static_cast<std::uint16_t>(pc + 2 * words);
    }
    // Target of a branch or SOB at 'pc'.
    [[nodiscard]] constexpr std::uint16_t branch_target(std::uint16_t pc) const noexcept {
        return static_cast<std::uint16_t>(pc + 2 + displacement);
    }
};

// Decodes the instruction whose opcode is w0, with the two words that follow
// it (used only as extension words, when the instruction has them).
[[nodiscard]] Instruction decode(std::uint16_t w0, std::uint16_t w1 = 0, std::uint16_t w2 = 0) noexcept;

// The vector an instruction traps through on the T-11, or -1.
[[nodiscard]] int trap_vector(const Instruction& insn) noexcept;

// MAME-format disassembly (octal), for an instruction at 'pc'.
[[nodiscard]] std::string disassemble(const Instruction& insn, std::uint16_t pc);

[[nodiscard]] const char* mnemonic(Op op) noexcept;

} // namespace qilin::t11
