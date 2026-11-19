# Recompiler core: status (roadmap step 2)

Last updated 2026-11-18 (v0.14.0: the lifter, and the differential tests against MAME's core; v0.13.0: control-flow analysis keyed by physical ROM address; v0.12.0: the project rebuilt to GBTGhoulies-Qilincomp's requirements, T-11 decoder verified against MAME).

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

**Heuristic tables are checked against the code.** A proof counts only if every slot in the range holds plausible code: a byte index read from RAM is provably 0-255, but its table is shorter, and the slots past it hold other data that can look like addresses. Such tables are sized by reading entries until the first implausible one. If an entry turns out to land inside an instruction found later, the table is cut there and the analysis runs again from scratch (two rounds on rev 7, 0.35 s in all).

**Results on rev 7:** 24,303 instructions in 659 functions, 5,315 in the fixed ROM and 18,988 across the 12 code banks; 53% of the fixed ROM's bytes and 63% of the code banks' bytes are proven code; 24 tables proven, 9 read; no code overlaps another instruction. Not followed statically, and listed by `qilin cfg --all`: 17 computed jumps (mostly through bank entry slots, `JMP @#40xx` with window 0's bank set at run time) and 25 computed calls (through RAM pointers, and the save helpers' `JSR PC,@(SP)+`). These dispatch at run time against the entries above and trap, naming the address and bank, on any target without lifted code. Also listed: 2 `BPT` instructions and 2 reads of `(PC)`.

## Lifter (`recomp/src/lifter.*`, `qilin lift`)
Each function becomes `void fn_8288(rt::Board&, unsigned entry)` (`fn_4012_b1` for code in a bank). Its body is every location reachable from its entry without crossing a call, emitted as labels and gotos; a location two functions share is emitted in both (5% of the 25,554 lifted instructions). The `entry` argument selects the function's start or one of its return sites, so the dispatcher can resume there.

**State.** R0-R6 and N, Z, V, C are locals, loaded on entry and written back to `Board::cpu` at calls, returns and service points (`QL_LOAD`, `QL_SYNC`). PC is a constant in the generated code; `cpu.r[7]` holds it only across those boundaries. The PSW's upper bits (priority, T) stay in `cpu.psw`.

**Instructions follow MAME's macros exactly** (`third_party/mame-t11/t11ops.hxx`), because the bus order is observable: the Slapstic watches every cycle.
- Operand addressing reproduces `MAKE_EA*`, `GET_S*`, `GET_D*` and `PUT_D*`, including where they differ: a source `#n` and an `@#a` operand are fetched (`ROPCODE`), but a destination `(PC)+` is read as data, and `JMP`/`JSR @#a` read their address word as data (`MAKE_EAW_IND`, which has no PC case). Byte autoincrement steps SP and PC by 2.
- `MOV`, `CLR`, `SXT`, `MOVB` and `CLRB` to memory read the destination before writing it, as the T-11 does in MAME (`PUT_DWT`, `PUT_DBT`); `MFPS` to memory doesn't.
- Flags use MAME's expressions on `int` values: a byte instruction's immediate source is the whole word, so `CMPB #imm` can carry from its high byte exactly as MAME's does.
- Every instruction charges MAME's cycles (`ic -= n`, before its accesses, as MAME does) and stores the count left in `b.icount` if it touches memory, so the board times its I/O by MAME's local time. Before every instruction, if the slice is used up, the state is written back and `Board::service()` runs (`--service-interval 0`, the default; `--service-interval N` checks only at back edges and after N cycles of straight code, faster but with events and interrupts landing later). After `RTI`, `RTT` and `MTPS`, where MAME checks the lines in the same step, `Board::check_interrupts()` takes one if it's due. A service that changes PC (an interrupt handler that doesn't return to the same place) or the code's bank makes the function return.

**Control flow.** Branches, `SOB` and proven tables are gotos; a table's `switch` falls back to leaving with the computed PC. `JSR` to a lifted function is a native call; other calls go through `rt::dispatch`, which finds the entry for PC under the current banks. After any call the caller checks it got back the address after the call (and its own bank, for window code); otherwise it returns too, leaving the state in `Board::cpu`, until a caller expects that address or the top-level dispatcher continues there. APB's save helpers need nothing more: a function calls the helper, the helper's `JSR PC,@(SP)+` enters the function again at its return site, the function's `RTS` comes back to the helper, and the helper's `RTS` returns to the function's caller, which each frame passes up one level. Traps push PSW and PC, take the vector and dispatch to the handler, which returns with `RTI`. `WAIT` hands over to the board until an interrupt.

**Fail loudly.** Forms the lifter doesn't handle (`-(PC)`, `@-(PC)`, `SOB PC`) stop `qilin lift` with the instruction and address, and nothing is written; dispatch to an address with no lifted code calls `Board::unlifted()`.

**Output:** 659 functions, 13 files, 8.6 MB of C++ in 0.6 s, warning-free under `-Wall -Wextra -Wpedantic -Wshadow -Werror` with GCC 14 and Clang 19.

## Differential test (`tools/difftest/`)
`qilin_difftest` builds one random state per trial: R0-R5 (RAM pointers, small counts, odd addresses, anything), SP in RAM with a return address that is never code on top, random flags, 16 KiB of random RAM, a code bank in window 0 (the function's own if it has one) and any bank in window 1. MAME's core runs the function until it returns; the lifted function runs from the same state. They must agree on R0-R6, the PSW, the cycles used, the banks, and the whole bus trace (every fetch, read and write with its address and value) in order, whole call trees included. RAM below 0x4000 is plain memory (devices aren't modelled: this tests the CPU), except that word writes to `0x1400-0x147F` select banks as on the board.

**Results on rev 7** (64 trials per function, 22 s): 42,176 trials, **32,698 match, 0 mismatch**; 592 of 659 functions complete at least one trial. Set aside, not failed: 3,109 trials that don't return within 200,000 steps (polling a device bit, which reads back random RAM here), 230 that wait for an interrupt, 752 that reach a dynamic target nothing was lifted for, 5,387 that the random state sends into RAM.

**Planted bugs:** dropping the dummy read before `MOV` to memory fails 516 functions, each report naming the first differing bus access. An off-by-one in `DEC`'s overflow condition is caught by no random game state, since a register rarely holds exactly 0x8000: that is what the lifter check is for.

## Lifter check (`tools/check/lifter_*.cpp`, in CI)
`qilin_lifter_fixture` writes a synthetic fixed ROM with one small function per instruction form and lifts it with the real analysis and lifter. The forms: all twelve two-operand operations with every source mode against every destination mode, registers varied, and the R7 forms on each side (`#n` with edge values, `@#a` even and odd, PC-relative into RAM, `@a` through a ROM pointer, `(PC)`); every one-operand operation, word and byte, in every mode; `XOR`; all 32 condition-code operations; `MFPT`, `RESET`; every branch, forward and backward; `SOB` on each register; `JMP` and `JSR` with each link register and fixed-target mode; the coroutine call `JSR PC,@(SP)+`; and `BPT`, `IOT`, `EMT`, `TRAP` and reserved instructions through vectors. `qilin_lifter_check` runs each against MAME's core 64 times from edge values (0, 1, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0x8001, 0xFFFF, RAM pointers, random words) in the registers, the flags and every RAM word, with return addresses all around SP so that forms which move SP are still compared.

**Results:** 2,322 forms, 148,608 trials, **145,606 match, 0 mismatch**; 2,302 forms complete at least once. The other 20 can't return by construction (they overwrite the return address, use SP as the link register, or are reserved instructions, after which the analysis rightly doesn't continue). 17 s in CI. The planted `DEC` bug fails 121 trials across its forms.

## The sound CPU (6502)
The 6502 program goes through the same stages, each checked against MAME's own 6502 (`third_party/mame-m6502`).

**Reference.** MAME's NMOS 6502 is cycle-exact microcode (`om6502.lst`) that MAME's `m6502make.py` turns into C++. The microcode, the generator and its output are vendored unmodified; `m6502_reference.*` runs them outside MAME's device framework behind a bus that sees every cycle. `qilin_mame_m6502_generated` regenerates the output where Python is found and checks the vendored copy.

**Decoder** (`recomp/src/m6502_decoder.*`): all 256 opcodes, undocumented ones marked, MAME's cycles and page penalties, control flow, MAME-format disassembly. `qilin_m6502_decoder_check` compares every opcode with MAME's disassembler and core: cycles with and without a page crossing, branches not taken, taken and taken across a page, one bus access per cycle, the PC afterwards, and the IRQ, NMI and reset sequences.

**Control flow** (`recomp/src/m6502_cfg.*`, `qilin cfg --cpu sound`). The sound program dispatches with the "RTS trick" (push a table entry, `RTS`) and hides data behind branches that are always taken, so exploration is driven by an abstract interpretation of A, X, Y, N/Z/C/V and the bytes each function has pushed:
- Values are exact sets of bytes. Each also records the earlier value it was computed from and its value for each of that one's values, so a branch on "the class read from `$5DE3,Y`" narrows Y, and with it the handler table index and the argument read from `$5EBC,Y`. That's how the main loop's dispatcher (196 commands, 15 handler classes, then a second table per handler) is listed exactly.
- A branch whose condition is known follows only its feasible edge; the lifted code still tests it and leaves through dispatch if it ever goes the other way.
- Callers' registers flow into callee entries (the sequencer calls its opcode handler only with `A >= 0x80`, after `BPL`).
- States are kept per stack depth, so an `RTS` shared by a plain return and a table jump (at `434B`) is both.
- Discovery and interpretation are one fixpoint over states that only grow, so every edge an execution from the vectors can take is followed. Table slots that don't decode as code are left out and reported.

**Result on rev 7:** 4,618 instructions (9.6 KB of the 48 KB ROM; the rest is music and speech data) in 66 functions, every table exact, nothing undocumented, no overlaps. One finding: at `42D9` an `RTS` runs with a byte still pushed, a latent bug in the game (the lifted code does what the chip does and goes through dispatch).

**Lifter** (`recomp/src/m6502_lifter.*`, `qilin lift --cpu sound`). Each instruction's body is MAME's microcode for its opcode, read from the embedded `om6502.lst` and translated token by token: `m_A` becomes the local `a`, `read(...)` a board read that counts a cycle, `do_adc` MAME's helper in `recomp/runtime/m6502_rt.hpp`. Anything the microcode uses that lifted code doesn't model (BRK's interrupt flags, KIL) stops `qilin lift`. So dummy reads, page-crossing reads and decimal arithmetic are MAME's by construction. The PC after an instruction is a local the compiler folds; control then goes to the label of the successor it equals. Calls are native with checked returns, as for the T-11, and table `RTS`es go to their targets' labels.

**Yielding.** The sound CPU runs inside the T-11's board service, so it must stop when its time slice ends, mid-function. Before every instruction (by default; `--service-interval N` as for the T-11), lifted 6502 code checks its cycles; the board may then take an interrupt (running the handler, which returns with `RTI`) or end the slice by setting `yielding`. Each bus cycle stores the count left in `b.icount` before the access, as MAME's microcode accesses and then counts. Around `CLI`, `PLP`, `SEI` and `RTI` the lifted code tells the board (`interrupt_window`), which decides on interrupts at the end of an instruction as MAME's core does, with the I flag from before `CLI`, `PLP` or `SEI` changed it. The lifted code then writes its state back and every frame returns. `rt6502::dispatch` resumes later at any function start, return site, service point or table target, and a function whose caller was unwound returns to dispatch, which continues at the return site. No threads, fibers or assembly.

**Difftest** (`qilin_sound_difftest`, needs your ROMs): every lifted function against MAME's core from random states (decimal mode one time in four), in random time slices of 1-400 cycles. 64 trials per function: 4,224 trials, **3,312 match, 0 mismatch**, 62 of 66 functions exercised, 238,378 yields and resumes. Set aside: 903 trials that wait on a device the test doesn't model, 9 where a random state sends the original into data. Planted bugs (decimal `ADC`, `PLA`'s dummy read) fail it.

**Lifter check** (`qilin_sound_lifter_check`, in CI): 245 synthetic functions, one per instruction form, run 96 times each from edge values in slices of 1-24 cycles: **23,520 trials, 0 mismatch**, every form completes. Planted bugs in `CMP`'s carry and in page-crossing detection fail it.

## What was replaced, and why
The first code (v0.1.0-v0.11.0) translated a reachable subset of T-11 instructions into one C++ function, with a label per instruction for every combination of PC and both bank registers. Its board models and instruction semantics were sound, but three things couldn't carry the full game:
- **Code keyed by bank state.** A runtime bank write expanded everything after it 64 times, and both windows switching at run time would give up to 4,096 copies of the fixed ROM. Code is now keyed by its physical ROM address (`docs/roadmap-and-design-targets.md`).
- **Per-instruction overhead.** Registers lived in a struct copied whole for each memory instruction, and every instruction checked a budget, polled interrupts and called the Slapstic. Lifted functions will keep registers in locals, compute only live flags, and service the board on cycle counts.
- **Coverage by hand.** About 30 instruction forms were decoded by pattern, jump tables were declared in a map file, and nothing checked the decoder. The decoder is now complete and verified, and analysis will size tables itself.

The read-only scan it ran on the real ROMs (209 locations from reset, stopping at the coroutine call `JSR PC,@(SP)+` at `0x81EA`) is the starting point for the new analysis.

## Next
1. The board and scheduler: System 2 behind `t11_rt.hpp` (scanline and VBLANK events, the watchdog, CP0-CP3, the sound latches and IN0) and the sound board behind `m6502_rt.hpp` (latch NMI, the 244 Hz IRQ, POKEY, YM2151 and TMS5220 status, the EEPROM), the sound CPU run in slices from the T-11's service.
2. A reference run: both CPUs on MAME's cores with those board models, from reset through the start-up handshake, recording every executed address; it confirms the T-11's dynamic targets (far calls through RAM pointers, bank entry slots) and `qilin coverage` accounts for every ROM byte.
3. `apb` running the lifted code of both CPUs from reset to the first frame.
