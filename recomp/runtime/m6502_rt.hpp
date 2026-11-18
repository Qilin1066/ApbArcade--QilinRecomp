#pragma once
// What lifted 6502 code (the sound CPU) relies on.
//
// Each guest function becomes  void snd_XXXX(qilin::rt6502::Board&, unsigned entry).
// Inside it A, X, Y, P, S and MAME's two scratch registers (TMP, TMP2) are
// host locals; at calls, returns and board service points they are written
// back to Board::cpu. Each instruction's body is MAME's own microcode for
// that opcode (om6502.lst), so the bus sees exactly the cycles MAME's core
// makes: opcode fetches, operand fetches, dummy reads, writes, one per cycle.
//
// Between instructions the next opcode has already been fetched (MAME's
// prefetch ends every instruction), so cpu.pc names the instruction to run
// next and its opcode fetch is behind it, as in MAME's m_NPC.
//
// The Board is supplied by whatever links the generated code, through the
// header "qilin_sound_board.hpp" on its include path: the System 2 sound
// board for the game, a tracing board for the tests. It must provide:
//
//   Cpu cpu;                        architectural state at boundaries
//   std::int32_t icount;            cycles left before the board needs service
//   bool yielding;                  set by service() to end the CPU's time slice
//   std::uint8_t read(std::uint16_t);        data read (dummy reads included)
//   std::uint8_t read_arg(std::uint16_t);    operand fetch
//   std::uint8_t read_sync(std::uint16_t);   opcode fetch
//   void write(std::uint16_t, std::uint8_t);
//   void service();                 runs device events and takes interrupts
//                                   (running the handler, which returns with
//                                   cpu.pc where it resumes), or ends the
//                                   slice by setting yielding; called with
//                                   cpu.pc = the instruction to resume at
//   void unlifted();                dispatch found no lifted code at cpu.pc:
//                                   report and stop
//
// When yielding is set, lifted code writes its state back and returns, all
// the way out; the board later resumes with dispatch(), which enters lifted
// code at any function start, return site or service point. A function whose
// caller has been unwound returns to dispatch the same way.
//
// The flag helpers below are MAME's (src/devices/cpu/m6502/m6502.cpp,
// BSD-3-Clause, copyright-holders: Olivier Galibert), as functions of the
// registers they change.
//
// Runtime code: no std::format (macOS 10.15), no exceptions required.

#include <cstdint>

namespace qilin::rt6502 {

struct Cpu {
    std::uint16_t pc = 0;    // the next instruction, its opcode already fetched
    std::uint8_t a = 0, x = 0, y = 0;
    std::uint8_t p = 0x36;   // as in MAME, B and bit 5 always read as set
    std::uint16_t sp = 0x01ff;
};

struct Board;   // defined by "qilin_sound_board.hpp"

using LiftedFn = void (*)(Board&, unsigned entry);

// A place dispatch can enter lifted code.
struct Entry {
    std::uint16_t pc;
    LiftedFn fn;
    unsigned index;
};

// Generated with the lifted code, sorted by pc.
extern const Entry lifted_entries[];
extern const unsigned lifted_entry_count;

// Enters lifted code at cpu.pc and returns when it returns or yields. False
// if nothing lifted starts there.
bool dispatch(Board& b);

enum : std::uint8_t {
    F_N = 0x80,
    F_V = 0x40,
    F_E = 0x20,
    F_B = 0x10,
    F_D = 0x08,
    F_I = 0x04,
    F_Z = 0x02,
    F_C = 0x01,
};

constexpr bool page_changing(std::uint16_t base, int delta) { return ((base + delta) ^ base) & 0xff00; }
constexpr std::uint16_t set_l(std::uint16_t base, std::uint8_t val) { return static_cast<std::uint16_t>((base & 0xff00) | val); }
constexpr std::uint16_t set_h(std::uint16_t base, std::uint8_t val) { return static_cast<std::uint16_t>((base & 0x00ff) | (val << 8)); }

inline void set_nz(std::uint8_t& p, std::uint8_t v) {
    p &= static_cast<std::uint8_t>(~(F_Z | F_N));
    if (v & 0x80) p |= F_N;
    if (!v) p |= F_Z;
}

inline void adc(std::uint8_t& a, std::uint8_t& p, std::uint8_t val) {
    if (p & F_D) {
        const std::uint8_t c = p & F_C ? 1 : 0;
        p &= static_cast<std::uint8_t>(~(F_N | F_V | F_Z | F_C));
        std::uint8_t al = static_cast<std::uint8_t>((a & 15) + (val & 15) + c);
        if (al > 9) al = static_cast<std::uint8_t>(al + 6);
        std::uint8_t ah = static_cast<std::uint8_t>((a >> 4) + (val >> 4) + (al > 15));
        if (!std::uint8_t(a + val + c)) p |= F_Z;
        else if (ah & 8) p |= F_N;
        if (~(a ^ val) & (a ^ (ah << 4)) & 0x80) p |= F_V;
        if (ah > 9) ah = static_cast<std::uint8_t>(ah + 6);
        if (ah > 15) p |= F_C;
        a = static_cast<std::uint8_t>((ah << 4) | (al & 15));
    } else {
        const std::uint16_t sum = static_cast<std::uint16_t>(a + val + (p & F_C ? 1 : 0));
        p &= static_cast<std::uint8_t>(~(F_N | F_V | F_Z | F_C));
        if (!std::uint8_t(sum)) p |= F_Z;
        else if (std::int8_t(sum) < 0) p |= F_N;
        if (~(a ^ val) & (a ^ sum) & 0x80) p |= F_V;
        if (sum & 0xff00) p |= F_C;
        a = static_cast<std::uint8_t>(sum);
    }
}

inline void sbc(std::uint8_t& a, std::uint8_t& p, std::uint8_t val) {
    if (p & F_D) {
        const std::uint8_t c = p & F_C ? 0 : 1;
        p &= static_cast<std::uint8_t>(~(F_N | F_V | F_Z | F_C));
        const std::uint16_t diff = static_cast<std::uint16_t>(a - val - c);
        std::uint8_t al = static_cast<std::uint8_t>((a & 15) - (val & 15) - c);
        std::uint8_t ah = static_cast<std::uint8_t>((a >> 4) - (val >> 4) - (std::int8_t(al) < 0));
        if (!std::uint8_t(diff)) p |= F_Z;
        else if (diff & 0x80) p |= F_N;
        if ((a ^ val) & (a ^ diff) & 0x80) p |= F_V;
        if (!(diff & 0xff00)) p |= F_C;
        if (std::int8_t(al) < 0) al = static_cast<std::uint8_t>(al - 6);
        if (std::int8_t(ah) < 0) ah = static_cast<std::uint8_t>(ah - 6);
        a = static_cast<std::uint8_t>((ah << 4) | (al & 15));
    } else {
        const std::uint16_t diff = static_cast<std::uint16_t>(a - val - (p & F_C ? 0 : 1));
        p &= static_cast<std::uint8_t>(~(F_N | F_V | F_Z | F_C));
        if (!std::uint8_t(diff)) p |= F_Z;
        else if (std::int8_t(diff) < 0) p |= F_N;
        if ((a ^ val) & (a ^ diff) & 0x80) p |= F_V;
        if (!(diff & 0xff00)) p |= F_C;
        a = static_cast<std::uint8_t>(diff);
    }
}

inline void cmp(std::uint8_t& p, std::uint8_t val1, std::uint8_t val2) {
    p &= static_cast<std::uint8_t>(~(F_N | F_Z | F_C));
    const std::uint16_t r = static_cast<std::uint16_t>(val1 - val2);
    if (!r) p |= F_Z;
    else if (std::int8_t(r) < 0) p |= F_N;
    if (!(r & 0xff00)) p |= F_C;
}

inline void bit(std::uint8_t a, std::uint8_t& p, std::uint8_t val) {
    p &= static_cast<std::uint8_t>(~(F_N | F_Z | F_V));
    const std::uint8_t r = a & val;
    if (!r) p |= F_Z;
    if (val & 0x80) p |= F_N;
    if (val & 0x40) p |= F_V;
}

inline std::uint8_t asl(std::uint8_t& p, std::uint8_t v) {
    p &= static_cast<std::uint8_t>(~(F_N | F_Z | F_C));
    const std::uint8_t r = static_cast<std::uint8_t>(v << 1);
    if (!r) p |= F_Z;
    else if (std::int8_t(r) < 0) p |= F_N;
    if (v & 0x80) p |= F_C;
    return r;
}

inline std::uint8_t lsr(std::uint8_t& p, std::uint8_t v) {
    p &= static_cast<std::uint8_t>(~(F_N | F_Z | F_C));
    if (v & 1) p |= F_C;
    v = static_cast<std::uint8_t>(v >> 1);
    if (!v) p |= F_Z;
    return v;
}

inline std::uint8_t ror(std::uint8_t& p, std::uint8_t v) {
    const bool c = p & F_C;
    p &= static_cast<std::uint8_t>(~(F_N | F_Z | F_C));
    if (v & 1) p |= F_C;
    v = static_cast<std::uint8_t>(v >> 1);
    if (c) v |= 0x80;
    if (!v) p |= F_Z;
    else if (std::int8_t(v) < 0) p |= F_N;
    return v;
}

inline std::uint8_t rol(std::uint8_t& p, std::uint8_t v) {
    const bool c = p & F_C;
    p &= static_cast<std::uint8_t>(~(F_N | F_Z | F_C));
    if (v & 0x80) p |= F_C;
    v = static_cast<std::uint8_t>(v << 1);
    if (c) v |= 0x01;
    if (!v) p |= F_Z;
    else if (std::int8_t(v) < 0) p |= F_N;
    return v;
}

} // namespace qilin::rt6502
