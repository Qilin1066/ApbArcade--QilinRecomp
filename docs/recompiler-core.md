# Recompiler core: status (roadmap step 2)

Last updated 2026-11-18 (v0.13.0: control-flow analysis keyed by physical ROM address; v0.12.0: the project rebuilt to GBTGhoulies-Qilincomp's requirements, T-11 decoder verified against MAME).

## Language and toolchain
Everything is **C++23**. Minimum compilers are GCC 14, Clang 19, Xcode 16 and Visual Studio 2022 17.10; CMake checks them at configure time. CI builds with warnings as errors (`-Wall -Wextra -Wpedantic -Wshadow`, MSVC `/W4`) on Linux x64 (GCC 14 and Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and x64, and Windows x64, and runs the renderer test on Mesa's software Vulkan driver with the validation layer.

## The reference CPU (`third_party/mame-t11`)
MAME's T-11 core is ground truth for every check, as Unicorn is for GBTGhoulies-Qilincomp's x86. Its instruction implementations (`t11ops.hxx`) and opcode table (`t11table.hxx`) are vendored unmodified and compiled inside a small class adapted from MAME's `t11.h` and `t11.cpp`, which runs outside MAME's device framework. Memory goes through a `Bus` interface, so a test sees every fetch, read and write in order. It counts cycles exactly as MAME does (`m_icount`). The differences from MAME's device are listed in `t11_reference.cpp`; none is visible on System 2 (no unaligned I/O-page handlers, no vectored interrupts).

## Decoder (`recomp/src/t11_decoder.*`)
Every 16-bit word decodes to an `Instruction`: operation, byte form, up to two operands (mode, register, extension word), length, MAME's cycle count, and whether the T-11 executes it. EIS (`MUL`, `DIV`, `ASH`, `ASHC`) and `MARK` decode for listings but are marked not legal: on the T-11 they trap through vector `010`, as the unassigned words do. `JMP` and `JSR` with a register destination trap through vector `004`.

**Cycle counts** follow MAME's tables, which reduce to a formula: 12 cycles, plus a source cost by addressing mode (0, 6, 6, 12, 9, 15, 15, 21 for modes 0-7), plus a destination cost that is the same for a read-only destination (`CMP`, `BIT`, `TST`) and 3 more for one that is written. `MTPS` is 24 plus its operand; `JMP` 15-27 by mode and `JSR` 12 more; branches 12; `SOB` 18; `RTS` 21; `RTI` 24; `RTT` 33; `RESET` 110; traps 48; an interrupt 114. MAME charges nothing for `WAIT` (it ends the timeslice) or `MFPT`; the decoder agrees, since MAME is the reference.

**Check** (`tools/check/t11_decoder_check.cpp`, in CI): for all 65,536 opcode words,
- the decoder's disassembly equals MAME's disassembler, character for character, at three PCs (including wrap-around at `0xFFFA`) with four sets of extension words;
- its length equals MAME's for every instruction the T-11 executes;
- running the word once in MAME's core takes exactly the decoder's cycle count;
- the word traps exactly when the decoder says it does, through the vector it names, with the frame MAME pushes.

All 65,536 agree; 56,872 of them execute on the T-11. A planted error in one cost entry, or one swapped mnemonic, fails the check.

## Board models (`recomp/runtime/system2.hpp`)
Kept from the earlier code after checking each against MAME's source, and now tested against MAME's own definitions rather than hand-picked cases (`recomp/tests`):
- **Bank select:** all 64 selector values against the table MAME keeps beside its bit swap.
- **Palette:** all 65,536 words against MAME's `RRRRGGGGBBBBIIII` formula.
- **Slapstic 137412-110:** a million bus cycles of random traffic, biased to the addresses the chip decodes, against a transcription of MAME's 103-110 state machine (direct, alternate and bitwise sequences, with their breaks): 0 mismatches over about 2,600 bank changes.
- **Interrupt latches, acknowledge ports, watchdog:** MAME's mirrors and timing.
- **Memory map:** `sys2::area()` decodes every address as MAME's `main_map` does, mirrors included.

## Control flow (`recomp/src/cfg.*`, `qilin cfg`)
Recursive descent from the reset entry (`0x8000`). A location is a CPU address and the bank selected in that address's window (0 in the fixed ROM): together they name one physical ROM word, and code in a bank exists once per bank. The two bank registers are not part of a location. Each location records what is known of them on entry, a bank number or unknown, joined over every path. They decide only two things: which bank a transfer into the *other* window lands in, and where the next instruction comes from after code switches its *own* window. Where the bank isn't known, the transfer is recorded for the runtime to dispatch on the bank, and listed.

A call (`JSR`) to a known target continues at its return site with the caller's own window unchanged and the other window unknown. A callee that switches the caller's window returns to a different bank; that is a return mismatch, which the runtime resolves (`docs/roadmap-and-design-targets.md`, *Calls and returns*).

**Function discovery,** in decreasing strength of evidence:
- the reset entry and direct `JSR` targets;
- **register-save prologues.** APB's routines open with `JSR Rn,@#helper`, where the helper (`0x81E2`, `0x81F2`, `0x8206`, `0x821E`) pushes registers and continues its caller with the coroutine call `JSR PC,@(SP)+`. Helpers are recognised by that structure, not by address, and every call to one is a function entry: 208 found;
- **bank entry tables.** Each code bank starts with a table of entry points into itself (0 for unused slots); the fixed ROM jumps and calls through its slots (`JMP @#40xx`, `JSR PC,@40000(Rn)`). The table ends at its first word that isn't an address in the bank's window, and can't run past the lowest address it lists: 98 entries;
- **pointer immediates** (`MOV #addr,...` into a register or RAM) and interrupt vectors installed by immediate stores. A constant can look like an address, so each must decode as plausible code (instructions the T-11 executes, through to a return or jump, without PC modes assemblers don't write) before it counts: 17 entries.

**Bank windows.** A bank's absolute `JSR`, `JMP` and pointer stores into a window say which window it runs in. Banks 0-2, 4-6, 8, 10 and 12-15 all refer to window 0 (`0x4000-0x5FFF`), and only to it; banks 3, 7, 9 and 11 and everything above bank 15 have no such references and are taken as data. The other window holds data the code reads.

**Jump tables** (`recomp/src/vrange.*`). `ADD X(Rn),PC` (offsets from the next instruction) and `JMP @X(Rn)` (addresses) are sized from the range value-range analysis proves for Rn. Registers R0-R5 are intervals taken modulo 65,536, so a decrement through zero stays exact. Moves, `CLR`, `INC`, `DEC`, `NEG`, `ASL`, `ASR`, `ADD` and `SUB`, `BIC` and `BIS` update them. A conditional branch narrows the register its flags came from (`CMP` with a constant either way round, `TST`, or the result of the last instruction, when V is known clear), on both edges, with signed and unsigned conditions. Calls and anything not modelled make a register unknown. Tables are re-proved every round as code is added, and a table whose index has widened beyond what the ROM supports is withdrawn. 25 are proven; 2 whose index is a byte read from RAM are sized by reading entries until the first implausible one, and the lifted code checks their index at run time.

**Results on rev 7** (0.14 s): 24,063 instructions in 659 functions, 5,315 in the fixed ROM and 18,748 across the 12 code banks; 53% of the fixed ROM's bytes and 62% of the code banks' bytes are proven code. Not followed statically, and listed by `qilin cfg --all`: 17 computed jumps (mostly through bank entry slots, `JMP @#40xx` with window 0's bank set at run time) and 25 computed calls (through RAM pointers, and the save helpers' `JSR PC,@(SP)+`). These dispatch at run time against the entries above and trap, naming the address and bank, on any target without lifted code. Also listed: 2 `BPT` instructions and 2 reads of `(PC)`.

## What was replaced, and why
The first code (v0.1.0-v0.11.0) translated a reachable subset of T-11 instructions into one C++ function, with a label per instruction for every combination of PC and both bank registers. Its board models and instruction semantics were sound, but three things couldn't carry the full game:
- **Code keyed by bank state.** A runtime bank write expanded everything after it 64 times, and both windows switching at run time would give up to 4,096 copies of the fixed ROM. Code is now keyed by its physical ROM address (`docs/roadmap-and-design-targets.md`).
- **Per-instruction overhead.** Registers lived in a struct copied whole for each memory instruction, and every instruction checked a budget, polled interrupts and called the Slapstic. Lifted functions will keep registers in locals, compute only live flags, and service the board on cycle counts.
- **Coverage by hand.** About 30 instruction forms were decoded by pattern, jump tables were declared in a map file, and nothing checked the decoder. The decoder is now complete and verified, and analysis will size tables itself.

The read-only scan it ran on the real ROMs (209 locations from reset, stopping at the coroutine call `JSR PC,@(SP)+` at `0x81EA`) is the starting point for the new analysis.

## Next
1. Coverage: account for every byte of the fixed ROM and the code banks (code, table, data, unknown), and a run-time trace from the reference CPU to confirm the dynamic targets.
2. The lifter: one C++ function per guest function, locals, flag liveness, cycle counting and board service points, checked returns with unwinding for coroutines.
3. The differential test: every lifted function against MAME's core on random states, comparing registers, flags, the bus trace in order, and cycles.
