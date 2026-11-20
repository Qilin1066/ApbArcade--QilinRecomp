# APB — Recomp Roadmap & Design Targets

The requirements and scope are those of GBTGhoulies-QilinRecomp, applied to an arcade board instead of an Xbox. Where the hardware differs, this document says how the same principle applies.

**Guiding principle: efficiency and reliability come first.** Every design choice is judged on whether it makes the game cheaper to run on low-end hardware and whether it fails loudly and predictably instead of silently misbehaving.

## Targets
- **Game:** *APB — All Points Bulletin* (Atari Games, 1987), the rev 7 ROM set (MAME `apb`). The board is Atari System 2: a DEC T-11 main CPU at 10 MHz, a 6502 sound CPU at 1.79 MHz, a YM2151, two POKEYs, a TMS5220 for speech, Slapstic 137412-110 video-memory banking, and a portrait (ROT270) 512 × 384 raster. Controls are a steering wheel and pedals, read through an ADC0809.
- **Platforms:** 64-bit Windows, Linux and macOS (Intel and Apple Silicon), on x86-64 or ARM64, with Vulkan. See *Hardware scope* below.
- **Language:** **C++23** throughout (recompiler, generated code and runtime). Minimum compilers: GCC 14, Clang 19, Xcode 16, Visual Studio 2022 17.10.
- **Graphics API:** **Vulkan 1.3, and only Vulkan**, with only the optional features and limits that Mesa's v3dv driver on a Pi 4 provides, so one render path works everywhere (MoltenVK on macOS). See *Vulkan baseline* below.
- **Reference:** MAME running the original ROMs is ground truth for behaviour, timing, video and audio. Its source is the specification wherever the hardware documentation is silent, and its T-11 core is vendored (`third_party/mame-t11`) as the reference CPU for every check.
- **Enhancement example target:** 3440 × 1440 (21:9) at 144 Hz, without changing game timing.

## Architecture
- **Game code:** static recompilation, T-11 and 6502 machine code → **generated C++**, compiled ahead of time for each target. No interpreter, no JIT. Hand-written C++ can replace a lifted function through an override table, and is checked against the lifted original.
- **Decoder:** our own, table-driven, for the full T-11 instruction set (EIS and MARK decode, and trap as on the chip). **Verified against MAME's disassembler and core on all 65,536 opcode words** in CI: disassembly, length, cycle count and traps (`qilin_t11_decoder_check`). The 6502's likewise, on all 256 opcodes (`qilin_m6502_decoder_check`).
- **Code identity:** a code location is its **physical ROM address** (an offset in MAME's `maincpu` region), never a CPU address paired with the bank registers. Code in the fixed ROM exists once, whatever the banks hold; code in a bank window exists once per bank it lives in. The bank registers are runtime state. Control that enters a window from outside it dispatches on the window's current bank to the lifted code for that bank, and traps with the bank and address if there is none. (The earlier design keyed code by PC and both banks, which copied the fixed ROM up to 4,096 times.)
- **Register model:** guest registers are host locals inside each lifted function, synced to the CPU state only at calls, returns and board service points. Flags are separate bool locals; a flag computation nothing reads before the next write is dead code the C++ compiler removes, so no flag is materialised except where it is read or written back. An explicit liveness pass is added only if profiling on the Pi 4 shows the need.
- **Calls and returns:** `JSR` to a known target is a native C++ call; `RTS` is a native return, and the caller checks the return address it gets back. A return anywhere else (coroutines such as `JSR PC,@(SP)+`, which APB uses at `0x81EA`, or a handler that rewrites its frame) unwinds to the dispatcher, which continues at that address if it is a known entry and traps with the address otherwise. The native stack never grows with coroutine switches.
- **Fail loudly:** an instruction that doesn't decode, an operand form the lifter doesn't handle, a computed target with no lifted code, or a bus access to an address nothing models is a diagnostic naming the instruction, its physical address and the banks — at build time where it can be known then, at run time otherwise. Never a guess.
- **Timing:** every instruction carries MAME's cycle count. Lifted code counts cycles in a local and, before each instruction, services the board (scanline and VBLANK events, the sound CPU, the watchdog, interrupts) once its slice is used up, so slices end, and interrupts are taken, on the instruction MAME's cores would. The board sees MAME's local time at its I/O. The native run and a run on MAME's cores give identical frames (`--frame-hashes`).
- **Bus effects:** reads and writes keep the T-11's bus order, including the dummy read MAME's T-11 makes before `MOV`, `CLR` and `SXT` write memory. The Slapstic watches every bus cycle (its alternate sequence starts on any address), so every access goes past it; the check is a branch on its state, folded away where the address is known not to matter.
- **Control flow:** recursive descent from the reset vector, `JSR` targets and the interrupt vectors the game installs; jump tables sized by analysis, not declared by hand. `qilin coverage` accounts for every ROM byte as code, table, data, padding or unknown, and exits non-zero while any unknown stretch decodes like code.
- **Board:** the System 2 devices are native C++ (`recomp/runtime/system2.hpp`), each checked against MAME's definition. Video is rendered from the board's RAM (playfield, motion objects, alphanumerics, palette) through Vulkan, not by emulating video hardware cycle by cycle.
- **Audio:** the 6502 program is recompiled the same way; the YM2151, POKEYs and TMS5220 are native C++ against MAME's models, mixed on a thread of their own and output through SDL3.
- **Input:** SDL3 for the keyboard, gamepads, wheels, pedals and mice (spinners). APB's steering is a free-spinning dial (LETA0), driven by how far a wheel's angle moves each frame, with an edge spin for wheels that stop at a lock; the gas pedal is ADC1. A settings menu (Dear ImGui) finds and calibrates wheels and pedals and binds any control to the cabinet's buttons and coins; force feedback gives the bound wheel friction and no centring spring. Settings and the EEPROM (2804) are kept in files.
- **Threads:** audio mixing and Vulkan submission off the game thread.
- **Portability rules:** no inline asm or intrinsics in the runtime; little-endian hosts; ARM64 builds from day one. The runtime avoids `std::format` (macOS 10.15); the developer tools may use it (macOS 13.3).

## Hardware scope
**Supported: 64-bit hosts with a Vulkan 1.3 driver:** Windows and Linux on x86-64 or ARM64; macOS on Intel and Apple Silicon through MoltenVK (macOS 10.15 or later for the runtime); ARM boards with Vulkan 1.3 drivers such as the Raspberry Pi 4 and newer (Mesa v3dv 24.3 or later).

**Out of scope:** anything without Vulkan 1.3, and 32-bit hosts.

### Vulkan baseline: 1.3
Vulkan 1.3 core is the floor; optional features and limits come from v3dv on a Pi 4. The renderer is shared with GBTGhoulies-QilinRecomp: dynamic rendering (no render pass objects), synchronization2, extended dynamic state, timeline semaphores for frame pacing. It checks what it uses at start-up and fails with a clear message rather than taking a fallback path.

**Low-end reference:** a Raspberry Pi 4–class board. It is what efficiency is measured against and tested on, not a floor.

## Settings, not platform branches
Enhancements are options in the settings file (and the menu), never mandatory; the original is the default. The Display page's presets:
- *Original:* the 512 × 384 raster upright, scaled by whole multiples and letterboxed, plain pixels, original 60.1 Hz timing.
- *Arcade monitor:* as big as fits, the CRT look.
- *Smooth:* as big as fits, smoothed pixel art.

## Roadmap
1. **Foundations** ✅ (v0.12.0): ROM set verified against MAME's digests, C++23 and CI on six configurations, the vendored Vulkan 1.3 renderer, the System 2 device models checked against MAME, MAME's T-11 core vendored as the reference CPU, and a full T-11 decoder verified against MAME on every opcode word.
2. **CPU coverage:** 🔨 ✅ bank-aware analysis keyed by physical address (v0.13.0); ✅ function discovery from the code's structure; ✅ jump tables by value range; ✅ the lifter (functions, locals, MAME's bus order and cycles, checked returns, v0.14.0); ✅ a **differential test of every lifted function against MAME's T-11 core**, and of every instruction form in CI (v0.14.0). ✅ the 6502 sound program the same way: decoder, analysis with exact RTS jump tables, a lifter whose instruction bodies are MAME's microcode, differential tests (v0.15.0). ✅ a reference-CPU run of the game (`qilin_refrun`) confirms dynamic targets, with a seeded random player and the self-test menus to reach the rest of the game. ✅ `qilin coverage` accounts for every ROM byte: with no seeds (v0.16.0: the analysis alone finds everything the reference runs execute), 100% of the T-11's code areas and 100% of the 6502's ROM explained as code, tables, data (referenced, indexed, pointed to, or read by a reference run) or fill, with no unknown byte; guessed code entries must look like code (no text, a balanced stack, no leading branch).
3. **Native startup:** the scheduler and board services; reach the first original frame with sound and input synchronized. Record the first unsupported address or board operation on every failure.
4. **Playable at original settings:** parity with MAME, cabinet controls, EEPROM, the device matrix; measure low-end performance before adding quality settings.
5. **Display settings:** 🔨 ✅ the settings menu's Display page (v0.16.0): full screen on any display, whole-multiple, fit or fill scaling, smooth or sharp, vsync, the graphics device. ✅ Looks: smoothed pixel art and a CRT, by fragment shaders; plain pixels stay a blit. 🔬 In-between frames (2×, 3×, 4× for high-refresh displays; one frame of delay; off by default): prototyped in `qilin_tween`, awaiting a decision. ✅ Presets: Original, Arcade monitor, Smooth. ✅ Speed: exact (60.1 Hz, the newest frame at each refresh; right for variable-refresh displays too) or matched to the display (60 Hz or a multiple, every frame in turn), with a check that the display really paces the game. Ruled out: a sideways (landscape) picture, which would hide the road ahead; and a wider picture filled from the board's off-screen map, which the game keeps current only 8-16 pixels beyond the edge.

Releases are tagged `vX.Y.Z`; `1.0.0` is the first build that plays the game end to end at original settings.
