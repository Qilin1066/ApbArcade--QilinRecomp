# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html), as in GBTGhoulies-Qilincomp:

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, a **patch** bump (0.x.**y**) a fix.
- **1.0.0** will be the first build that plays the game end to end at original settings.

Each release is a git tag (`vX.Y.Z`). The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. Entries before 0.12.0 predate this format and are kept as written.

## [Unreleased]

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
