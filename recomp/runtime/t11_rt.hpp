#pragma once
// What lifted T-11 code relies on.
//
// Each guest function becomes  void fn_XXXX(qilin::rt::Board&, unsigned entry).
// Inside it, R0-R6 and the N, Z, V and C flags are host locals; at calls,
// returns and board service points they are written back to Board::cpu, the
// architectural state. R7 (the PC) is a compile-time constant inside lifted
// code; Board::cpu.r[7] holds it only across those boundaries.
//
// The Board is supplied by whatever links the generated code, through the
// header "qilin_board.hpp" on its include path: the System 2 board for the
// game, a tracing board for the differential test. It must provide:
//
//   Cpu cpu;                       architectural state at boundaries
//   std::int32_t icount;           cycles left before the board needs service
//   std::uint8_t bank[2];          the ROM bank in each window (0x4000, 0x6000)
//   void fetch(std::uint16_t);     an instruction-stream fetch (opcode or extension word)
//   std::uint16_t read16(std::uint16_t);              word read (address even)
//   void write16(std::uint16_t, std::uint16_t);       word write (address even)
//   std::uint8_t read8(std::uint16_t);                byte read
//   void write8(std::uint16_t, std::uint8_t);         byte write
//   void service();                runs device events and takes interrupts; called
//                                  with cpu.r[7] = the PC to resume at
//   void wait();                   WAIT: until an interrupt has been taken
//   void reset_output();           RESET: the T-11's reset line pulse
//   void unlifted();               dispatch found no lifted code at cpu.r[7] and
//                                  the current banks: report and stop
//
// Bus calls happen in exactly the order MAME's T-11 core makes them, fetches
// included, so a board can model devices that watch the bus (the Slapstic).
//
// Runtime code: no std::format (macOS 10.15), no exceptions required.

#include <cstdint>

namespace qilin::rt {

struct Cpu {
    std::uint16_t r[8] = {};   // R0-R5, SP, PC
    std::uint8_t psw = 0;      // priority (bits 5-7), T (bit 4), N Z V C (bits 3-0)
};

struct Board;   // defined by "qilin_board.hpp"

using LiftedFn = void (*)(Board&, unsigned entry);

// A place dispatch can enter lifted code: a function's start or a point it
// resumes at (the instruction after a call). 'bank' is 0 in the fixed ROM.
struct Entry {
    std::uint16_t pc;
    std::uint8_t bank;
    LiftedFn fn;
    unsigned index;
};

// Generated with the lifted code, sorted by (pc, bank).
extern const Entry lifted_entries[];
extern const unsigned lifted_entry_count;

// Enters lifted code at cpu.r[7] under the current banks and returns when it
// returns. False if nothing lifted starts there.
bool dispatch(Board& b);

} // namespace qilin::rt
