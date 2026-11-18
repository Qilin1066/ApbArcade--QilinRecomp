# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html), as in GBTGhoulies-Qilincomp:

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, a **patch** bump (0.x.**y**) a fix.
- **1.0.0** will be the first build that plays the game end to end at original settings.

Each release is a git tag (`vX.Y.Z`). The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. Entries before 0.12.0 predate this format and are kept as written.

## [Unreleased]

Known difference from MAME: interrupts are taken at service points, so where the game races an interrupt the outcome can differ from MAME's run. The first seen: the sound program's main loop clears bit 0 of its status byte with a non-atomic `LDA/AND/STA`; in MAME an NMI landed in between and the handler's status bits were lost, natively it doesn't, so one later status reply is 04 instead of 00. Either is something the hardware can do.

### Added
- **The System 2 board as both CPUs see it** (`recomp/runtime/system2_machine.*`, library `qilin_board`), after MAME's `atarisy2.cpp`: the main map (RAM, palette, ROM banks, the Slapstic video view, CP0-CP3 latched from the interrupt enable, which applies at the next synchronisation as MAME's `synchronize` does, IN0 with the two latches' pending bits, the watchdog, the ADC) and the sound map with all of MAME's mirrors (RAM, EEPROM, two POKEYs, the YM2151's busy flag, the TMS5220, IN1, the LETA dial, the latches, the IRQ acknowledge). Events in main-CPU cycles, all whole numbers: the 32V interrupt every 64 lines, VBLANK at line 384 (166,400 cycles a frame), the 244 Hz sound IRQ (40,960 cycles, never while the 6502 is held in reset) and the watchdog (1,048,576). Unit-tested against MAME's definitions.
- **`qilin_refrun`** (`tools/refrun/`, needs your ROMs): the game on MAME's own T-11 and 6502 cores and that board, in 64-cycle slices, with every executed address recorded; coins, buttons, the pedal and the steering dial can be scripted. On rev 7 it runs the power-on tests, releases the sound CPU, completes the start-up handshake and runs attract mode and a credited game with no watchdog reset, two minutes of game time in 7 s. It compares what ran with both analyses and turns what the analysis missed into seeds (`--seeds-out`), re-analysing until everything executed is covered: 36 seeds over three runs, all of them entries reached only through RAM handler tables, pointers in data records or far-call trampolines. Every 6502 address executed was already found.
- **`--seeds FILE`** for `qilin cfg` and `qilin lift`.
- **The game's scheduler** (`recomp/runtime/game/`): both CPUs' lifted code on the board model. The T-11 leads; at each of its service points the machine's clock advances, the 6502's lifted code catches up in a slice it ends by yielding, the board's events run, and a pending T-11 interrupt is taken, its handler running natively, as MAME takes them at the start of a timeslice. Interrupt sequences make MAME's bus accesses and charge its cycles (T-11 114, 6502 7, 6502 reset 8). Stops (frame count, watchdog, unlifted code) end the run with the reason.
- **`qilin_run`** (`tools/run/`, needs your ROMs and both lifts): runs the game natively with the reference run's scripted inputs. On rev 7, attract mode and a credited game each run 20,000 frames (5.5 minutes of game time) in under 6 s, about 60 times real time on one core, with no unlifted code and no watchdog reset: 112,918 sound commands, 155,215 main-CPU interrupts. `--log` and `qilin_refrun --log` write the latch, IRQ and reset traffic of a run for comparing the two.

### Fixed
- **A sound command could lose its NMI.** The scheduler looked at the latch's line only between 6502 slices, so a command read and the next one written in between left it high both times and no edge was seen; the 6502 then waited forever for a byte. The machine now counts the line's rising edges (a command written to an empty latch, as MAME's latch asserts it), and every one is taken.
- **The T-11 lifter left `JMP @X(Rn)` tables through dispatch**, where their targets have no entry; they now go to their targets' labels, as `ADD X(Rn),PC` tables do. Both kinds of table jump also service the board when cycles run out, since a loop can run through one.
- **Lifted T-11 code can be re-entered wherever it may have been interrupted:** at service points and table targets as well as function starts and return sites. APB's interrupt handler switches tasks, returning to code interrupted earlier, elsewhere.
- **`JMP @X(Rn)` jump tables were sized but never applied**: exploration only used resolved tables for `ADD X(Rn),PC`. The reference run found the one at `8820` by executing it.
- **Value ranges narrow on `BIT #mask,Rn`** where the mask is a run of high bits (`BIT #177740,R1 / BEQ`: R1 < 32), which proves that table.
- **An entry in a window is explored with its own bank known.** Seeds and pointer entries in banked code used to advance one instruction per analysis round.
- **Pointer immediates into the code's own window** (`MOV #4D34,52A(R0)` in bank 12) are entries in that bank. Guessed pointers must now look like ordinary code: no traps or `RTT` and no repeated identical branch, which rejects two data tables in bank 2 that had been decoded as code.
## [0.15.0] - 2026-11-18
The sound CPU. The 6502 program gets everything the T-11's has: MAME's core as the reference, a decoder verified on every opcode, control-flow analysis that resolves every jump table exactly, and a lifter whose instruction bodies are MAME's own microcode, checked against MAME's core on the game's functions (3,312 trials) and on every instruction form (23,520 trials). 0 mismatches.

### Added
- **MAME's 6502 core as the reference sound CPU** (`third_party/mame-m6502`, BSD-3-Clause): MAME's opcode microcode (`om6502.lst`), its generator and the generated, cycle-exact `m6502.hxx`, unmodified, behind a bus interface that sees every bus cycle (opcode fetches, operand fetches, dummy reads, writes). `qilin_mame_m6502_generated` (CI, where Python is found) regenerates the sources and checks the vendored copies match.
- **A full 6502 decoder** (`recomp/src/m6502_decoder.*`): all 256 NMOS opcodes, the undocumented ones marked as such, MAME's cycle counts and page-crossing penalties, control flow, MAME-format disassembly.
- **`qilin_m6502_decoder_check`** (CI): disassembly, length and step flags of all 256 opcodes against MAME's disassembler; cycles (with and without a page crossing; branches not taken, taken, and taken into the next page), one bus access per cycle, and where the PC goes against MAME's core; IRQ, NMI and reset sequences. All agree.
- **Control flow of the sound program** (`recomp/src/m6502_cfg.*`, `qilin cfg <rom-dir> --cpu sound`): recursive descent from the reset, NMI and IRQ vectors driven by an abstract interpretation of A, X, Y, the flags and the bytes each function pushes. Values are exact byte sets that remember which other value they were computed from, so a branch narrows the command byte through the table that was indexed with it. That prunes always-taken branches in front of data and lists every target of APB's "RTS trick" dispatchers exactly, the main loop's command dispatcher (196 commands through 15 handlers, then a second table) and the NMI's included. Callers' registers flow into their callees; an `RTS` that returns on one path and jumps on another is kept apart by stack depth. From the vectors: 4,618 instructions, nothing undocumented, no overlaps, every table slot code. One finding: an `RTS` at `42D9` with a byte still pushed, a latent bug in the game.
- **`qilin insns <rom-dir> --cpu sound`** lists 6502 code in MAME's syntax.
- **The 6502 lifter** (`recomp/src/m6502_lifter.*`, `qilin lift <rom-dir> --cpu sound --out DIR`): one C++ function per sound-program function, `snd_XXXX`. Each instruction's body is MAME's own microcode for its opcode, read from the vendored `om6502.lst` (built into `qilin`) and translated token by token, so the bus sees MAME's cycles in MAME's order, dummy and page-crossing reads included, one cycle each. The PC after each instruction folds to a constant; an RTS through a table goes straight to its targets; calls are native with checked returns. All 66 functions lift (5,911 instructions, shared code counted per function).
- **Yielding:** lifted 6502 code checks for board service at back edges, table dispatches and long straight runs; the board can end the CPU's time slice there, and the code unwinds and later resumes through `rt6502::dispatch`, which enters at function starts, return sites and service points. This is how the sound CPU will interleave with the T-11 without threads or fibers.
- **`recomp/runtime/m6502_rt.hpp`:** the contract between lifted 6502 code and its board, with MAME's flag and decimal-arithmetic helpers.
- **`qilin_sound_difftest`** (`tools/difftest6502/`, needs your ROMs; build with `-DQILIN_SOUND_GEN_DIR`): every lifted sound function against MAME's 6502 core from random states (decimal mode included), in time slices of random length so the lifted code also yields mid-function. 4,224 trials: 3,312 match, 0 mismatch, 62 of 66 functions exercised, 238,378 yields and resumes. Planted bugs in decimal `ADC` and in `PLA`'s dummy read both fail it.
- **`qilin_sound_lifter_check`** (CI, no game files): a synthetic sound ROM with one function per instruction form (every documented opcode in every mode, operands in RAM and ROM, page crossings, branches both ways and across a page, a counted loop, `JMP` absolute and indirect with the NMOS page wrap, `JSR`/`RTS`, the stack instructions, RTS through constant pushes and through an indexed table, NMI and IRQ handlers), lifted by the real lifter and run against MAME's core from edge values, decimal mode included, in time slices of 1-24 cycles. 245 of 245 forms complete; 23,520 trials, 0 mismatches, 14,957 yields and resumes. A compare that sets carry wrongly on equality, and page crossings never detected, both fail it.

## [0.14.0] - 2026-11-18
The lifter. All 659 functions the analysis finds lift to C++, and every one is checked against MAME's T-11 core: 32,698 trials on the game's own functions and 145,606 on every instruction form, with registers, flags, cycles and the whole bus trace in order compared. 0 mismatches.

### Added
- **`qilin lift <rom-dir> --out DIR`** (`recomp/src/lifter.*`): one C++ function per guest function, `fn_8288` or `fn_4012_b1`, its body every location reachable without crossing a call, as labels and gotos. R0-R6 and the flags are locals; PC is a constant. Each instruction makes its bus calls in the order MAME's core does, fetches included (the dummy read before `MOV`/`CLR`/`SXT` writes, a destination `(PC)+` read as data, `JMP`/`JSR @#a` reading its address word), charges MAME's cycle count, and computes its flags with MAME's own expressions. 25,554 instructions in 0.6 s; code shared by several functions is copied into each (5% more).
- **Calls and returns as native C++**, each caller checking the address (and, in a window, the bank) its callee returned to, and returning too when it differs. APB's register-save helpers, which jump back into their caller with `JSR PC,@(SP)+`, run this way with no special case. A dispatcher (`rt::dispatch`) enters lifted code at any function start or return site under the current banks, for computed calls and jumps.
- **Board service points:** cycles are counted in a local; when they run out at a loop's back edge or after a straight run (at most 128 cycles), the state is written back and `Board::service()` runs device events and interrupts. Also after `MTPS` and traps, where MAME checks interrupts.
- **`recomp/runtime/t11_rt.hpp`:** the contract between lifted code and the board that runs it (memory, fetches, banks, service, dispatch).
- **`qilin_difftest`** (`tools/difftest/`, needs your ROMs; build with `-DQILIN_GEN_DIR`): every lifted function against MAME's core from random states (registers, flags, 16 KiB of RAM, banks), whole call trees included. 42,176 trials: 32,698 match, 0 mismatch, 592 functions exercised; the rest are set aside (they wait on hardware the test doesn't model, or leave the lifted set).
- **`qilin_lifter_check`** (CI, no game files): a synthetic program with one function per instruction form (every two-operand operation with every source and destination mode, the R7 forms on each side, every one-operand operation and mode, branches both ways, `SOB`, `JMP` and `JSR` forms, the coroutine call, traps), lifted by the real lifter and run against MAME's core from edge values (0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF ...). 2,302 of 2,322 forms complete; 145,606 trials, 0 mismatches. A planted off-by-one in `DEC`'s overflow flag, which random game states never catch, fails it.

### Fixed
- **Jump tables:** a proof now counts only if every slot in the proven range holds plausible code. A byte index read from RAM is provably 0-255 but its table is shorter; such tables are sized by reading entries, and one whose entry turns out to land inside an instruction is cut there and the analysis re-run.
- **Bank-window inference** counted `MOV #a,@#x` pointer stores under a mask that could never match (found by Clang 19).


## [0.13.0] - 2026-11-18
Control-flow analysis keyed by physical ROM address. From reset it reaches 24,063 T-11 instructions in 659 functions, in the fixed ROM and all 12 code banks, with every jump table sized by analysis; nothing is declared by hand.

### Added
- **`qilin cfg <rom-dir> [--all]`** (`recomp/src/cfg.*`): recursive descent from reset. A location is a CPU address plus the bank of its own window; the bank registers are tracked as known constants or unknown, never multiplied into the code. Everything it can't follow is listed with its address and banks: computed jumps and calls, transfers into a window whose bank isn't known, traps, unusual PC modes.
- **Value-range analysis** (`recomp/src/vrange.*`): R0-R5 as intervals modulo 65,536, through moves, arithmetic, shifts, BIC and BIS, narrowed on both edges of every conditional branch (signed and unsigned). It sizes `ADD X(Rn),PC` and `JMP @X(Rn)` tables: 25 proven, among them the start-up table at `0xF15E` that the old map file declared. Tables are re-proved as the program grows, so one proven on part of the code is withdrawn if more code widens its index.
- **Function discovery from the code's own structure:** the register-save helpers (found structurally: a `JSR Rn,@#H` target that continues its caller with `JSR PC,@(SP)+`) and the 208 prologues that call them; the entry table each code bank starts with (98 entries); pointer immediates (`MOV #addr`), each validated by decoding before it counts; interrupt vectors installed by immediate stores.
- **Bank windows inferred from the ROM:** each bank's absolute references to itself say which window it runs in. Banks 0-2, 4-6, 8, 10 and 12-15 run in window 0; the rest are data.
- **Heuristic jump tables** where the index comes from memory: entries read until the first implausible one, marked as such; the lifted code will check every index at run time.
- **Unit tests for the analysis** on synthetic ROMs: a loop-indexed table proven to its exact size, prologue seeding, bank-window inference, and an unknown-bank entry reported.

### Fixed
- A table proven before a loop's back edge was explored kept its first, too-small size. Tables are now re-proved every round.


## [0.12.0] - 2026-11-18
The project is rebuilt to GBTGhoulies-Qilincomp's requirements and layout. The T-11 decoder is complete and verified against MAME on every opcode word. The earlier translator is retired; `docs/recompiler-core.md` says why.

### Added
- **`qilin`**, the recompiler CLI: `qilin verify <rom-dir>` checks all 36 rev 7 files (size and SHA-1, in C++ instead of a CMake script); `qilin insns <rom-dir>` lists T-11 code at CPU addresses under chosen banks, in MAME's syntax with cycle counts.
- **The ROM set loader** (`recomp/src/romset.*`): MAME's complete `apb` layout for all five regions (byte lanes, `ROM_CONTINUE` pieces, the inverted sprite region, zero fill), with every problem named.
- **A full T-11 decoder** (`recomp/src/t11_decoder.*`): every instruction the chip executes, EIS and `MARK` decoded as traps, MAME's cycle counts, MAME-format disassembly.
- **MAME's T-11 core as the reference CPU** (`third_party/mame-t11`, BSD-3-Clause): MAME's opcode files unmodified, behind a bus interface that traces every cycle and counts cycles as MAME does.
- **`qilin_t11_decoder_check`** (CI): disassembly, length, cycles and traps of all 65,536 opcode words against MAME's disassembler and core. All agree.
- **Board model tests against MAME's definitions:** the bank table, the palette formula on every value, and the Slapstic 110 on random bus traffic against a transcription of MAME's state machine (0 mismatches).
- **The System 2 memory map** as code (`sys2::area`), from MAME's `main_map`, mirrors included.
- **Design documents:** `docs/roadmap-and-design-targets.md` (the Ghoulies requirements as they apply to System 2), `docs/system2.md` (the board, from MAME), `docs/recompiler-core.md`, `docs/naming.md`.

### Changed
- **Layout and naming follow GBTGhoulies-Qilincomp:** `recomp/src`, `recomp/runtime`, `recomp/tests`, `tools/check`, `third_party`; `qilin` and `QILIN_*` in place of `apb-t11-aot` and `APB_*`.
- **The renderer is GBTGhoulies-Qilincomp's** Vulkan 1.3 renderer (dynamic rendering, synchronization2, volk loading, validation-checked in CI), without depth and stencil, which 2D System 2 video doesn't use.
- **Vulkan headers and volk are vendored**, so a clone builds offline; configure no longer downloads anything.
- **CI** runs on every push, with the same six configurations as GBTGhoulies-Qilincomp.

### Removed
- **The PC-and-bank translator** (`apb-t11-aot`), its ROM and jump-table map files, and the CMake ROM verifier. Code generation is being rebuilt around physical ROM addresses (`docs/roadmap-and-design-targets.md`).

## 0.11.0

- Compile MOVB, BISB, indexed byte clears, and word predecrement for the banked CPU. Read mapped ROM data using the active banks for register-based byte and word operands.
- Model the main CPU's sound-reset output and command and reply ports, including latch status and interrupt acknowledgements.
- Compile direct absolute and PC-relative subroutine calls with PC or R0–R5 as the link register. Test nested calls, stack writes, and register restoration across all 64 bank selections.
- Advance the verified rev 7 reset scan to the indirect call at 0x81EA after 209 PC and bank combinations.

## 0.10.0

- Compile System 2 ROM bank selector writes with runtime operands and dispatch to precompiled bank contexts.
- Verify all 64 hardware selector values, instruction fetch across a bank change, and missing-ROM diagnostics.
- Advance the read-only rev 7 scan past the bank selector at PC 0xF2C4 to MOVB at PC 0xF2D4.

## 0.9.0

- Resolve declared indexed ROM jump tables into native branches and diagnose out-of-range table accesses.
- Compile word BIT, BIC, and BIS and test the result and condition flags in a synthetic native program.
- Resolve four rev 7 targets at 0xF15E and advance the read-only scan to the register-sourced bank selector write at 0xF2C4.

## 0.8.0

- Compile indexed-deferred T‑11 word operands with aligned pointer and data bus accesses.
- Distinguish an unsupported pointer fetch from an unsupported final access.
- Test native indexed-deferred reads and writes and advance the read-only rev 7 scan to the computed transfer at PC 0xF15E.

## 0.7.0

- Map the 256-word System 2 palette and its mirror in the native bus.
- Decode palette RGB and shared intensity, and test mirrored reads, word writes, and byte clears.
- Compile register negate and arithmetic shift left.
- Advance the read-only rev 7 scan to the indexed-deferred operand at 0xF156.

## 0.6.0

- Track Slapstic 110 selection and route the System 2 video window to separate pages.
- Feed instruction fetches and supported data accesses to the board’s video selector in bus order.
- Compile indexed word operands and register-based word clears.
- Verify native video-memory access and advance the read-only rev 7 scan to the palette write at PC 0xF07E.

## 0.5.0

- Support register-deferred and autoincrement word operands in low RAM and a literal destination for comparisons.
- Preserve source-before-destination evaluation and align T‑11 word data accesses.
- Report out-of-range dynamic reads and writes before committing the current instruction.
- Advance the read-only rev 7 scan to the Slapstic read at PC 0xF062.

## 0.4.0

- Model the System 2 interrupt mask, control-pin latches, and mirrored acknowledgement writes.
- Accept eligible T‑11 interrupts in native code, save the return frame, and report the vector and handler PC.
- Test mask and latch semantics, acknowledgement mirrors, IRQ priority, and stack validation.
- Advance the read-only rev 7 scan to the autoincrement operand at PC 0xF03C.

## 0.3.0

- Lower System 2 watchdog strobes from native byte and word clears and word moves.
- Count host-supplied board ticks, latch expiry, and stop native entry for a pending reset.
- Verify the watchdog mirrors and reset behavior with synthetic native code.
- Advance the read-only rev 7 scan to the interrupt-enable write at 0xF034.

## 0.2.0

- Load System 2 T‑11 ROM maps from split byte lanes and diagnose missing or overlapping regions.
- Compile direct jumps and immediate bank selections ahead of time, including switches that fetch an operand from the other ROM window.
- Lower absolute RAM and ROM word operands, plus byte clears in low RAM.
- Inspect the verified rev 7 reset path and report its first unsupported watchdog access at 0xF030.
- Test banked execution and map diagnostics with hand-authored fixtures.

## 0.1.1

- Configure Vulkan headers without the upstream C++ module target so GCC 14 can build with Unix Makefiles.

## 0.1.0

- Establish the 64-bit C++23 and Vulkan 1.3 project and its build matrix.
- Verify all 36 ROMs in Atari's arcade rev 7 set against published sizes and digests.
- Translate a bounded subset of T‑11 code ahead of time and test native execution using a synthetic program.
- Clear and read back an offscreen Vulkan image through the runtime loader.
