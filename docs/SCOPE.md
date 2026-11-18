# Scope and roadmap

The release goal is a fast, reliable, native build using the owner's Atari arcade rev 7 files. The same C++23 code targets x86‑64 and ARM64. One Vulkan 1.3 renderer uses the features shared by the Raspberry Pi 4 and MoltenVK. Pi 4 is the reference for low-end performance. The macOS runtime targets Catalina on Intel and uses the architecture's supported floor on Apple Silicon.

The build translates the T‑11 and 6502 code ahead of time. The shipped host runs compiled functions, with native services for the board and a diagnostic for a missing operation or unresolved target. Validate any hand-written overrides against the lifted code. MAME runs the original ROMs as a development reference for instruction traces, interrupts, video, and audio. Repository history contains source, tests, documentation, and build definitions. Every release gets a changelog entry and a tag of the form `vX.Y.Z`. `1.0.0` marks the first playable release.

## Arcade adaptation

| Area | Scope |
| --- | --- |
| Input and analysis | Verify the original split ROMs. Describe the fixed and banked T‑11 address spaces and the separate 6502 address space. Check reachable instructions, branch destinations, indirect targets, and what ROM data the executable reads |
| Native CPU code | Generate C++23 for both CPUs before launch. Compare register state, bus effects, and interrupt ordering against development traces. Compile known targets and report unsupported instructions with their bank and PC. Use flag liveness and keep register state local to translated code where profiling shows a benefit |
| Board services | Provide the System 2 memory-mapped services, bank selection, video RAM, device timing, the sound CPU interface, and EEPROM persistence. Model shared timing so frame delivery, audio, and input agree |
| Video | Keep the cabinet's portrait-oriented presentation and timing as the default. Render the playfield, text, and motion objects through Vulkan 1.3. Allow integer scaling and letterboxing. Measure high-refresh presentation without changing game timing |
| Audio and controls | Preserve sound command timing and cover the cabinet's audio and speech hardware. Support wheel and pedal input as well as keyboard and gamepad bindings, with the cabinet switch and service functions available |
| Validation | Run ROM-independent tests in CI. Compare local boot and game traces with MAME. Measure frame pacing, audio underruns, and input behaviour on a Pi 4, then verify other desktop targets and a Metal path |

## Milestones

1. **Project foundation.** Verify rev 7, run a generic T‑11 program compiled ahead of time, exercise Vulkan offscreen, and configure portability CI.
2. **CPU coverage.** Build bank-aware T‑11 and 6502 analysis. Validate emitted code with synthetic tests and local differential traces. Track every reachable byte as code, data, padding, or unknown.
3. **Native startup.** Connect board services and reach the first original frame with synchronized sound and input. Record the first unsupported address or board operation on every startup failure.
4. **Playable at original settings.** Establish baseline parity, cabinet controls, EEPROM behaviour, and the device matrix. Measure low-end performance before raising quality settings.
5. **Optional display settings.** Use presets for cabinet-faithful and desktop output. Evaluate integer scaling, larger output sizes, and high-refresh presentation against the original portrait layout and timing.

## System 2 checkpoint

The T‑11 compiler tracks the PC and the physical bank selected in each ROM window. The hardware exposes 8 KiB windows at 0x4000–0x5FFF and 0x6000–0x7FFF, and fixed ROM at 0x8000–0xFFFF. The generated code follows distinct precompiled paths through a bank change, including when the last operand is fetched from the other window. Absolute ROM operands use the selected ROM word and issue a bus observation during native execution.

The watchdog rearms on the low byte of any word in 0x1800–0x1BFF. The port also accepts aligned word writes through that byte lane. Its period is 1,048,576 main-board clock ticks, or 104.8576 ms at 10 MHz. A host-supplied tick count advances the timer. Expiry latches a reset request and the next native entry stops before executing an instruction. Instruction timing and scheduling the board reset remain pending. [Atari System 2 board driver](https://github.com/mamedev/mame/blob/master/src/mame/atari/atarisy2.cpp) · [MAME watchdog device](https://github.com/mamedev/mame/blob/master/src/devices/machine/watchdog.cpp)

Board events latch their enable bits onto CP0–CP3. Mask writes preserve the latched lines. The mirrored acknowledgement ports clear the sound command, scanline, or vblank latch. The sound reply latch clears when the main CPU reads the reply. The current controller exposes this acknowledgement for the future sound latch service. The probe handles MMIO writes at the CPU instruction boundary and takes device events from the host. [Atari System 2 board driver](https://github.com/mamedev/mame/blob/master/src/mame/atari/atarisy2.cpp)

An eligible IRQ selects a vector from the combined pin code and compares its priority to the current PSW. Native entry saves the interrupted PC and PSW in low RAM, loads the vector PSW, and returns the handler PC and active ROM banks. Scheduled scanline, vblank and sound events, native handler dispatch, and return from interrupt remain pending. [MAME DEC T‑11 core](https://github.com/mamedev/mame/blob/master/src/devices/cpu/t11/t11.cpp)

Register-based word operands reach low RAM, palette RAM, and the video window. Code captures the source before resolving the destination, ignores bit zero for each word bus access, and advances the register by two for each autoincrement operand. Indexed-deferred addressing reads the pointer at the register plus displacement, then checks the pointed-to address separately. An unsupported dynamic address reports the PC, banks, and word address before committing the instruction’s register, memory, or flag changes. A read through `(PC)+` in a comparison consumes the following literal word. [MAME DEC T‑11 operand reference](https://github.com/mamedev/mame/blob/master/src/devices/cpu/t11/t11ops.hxx)

The System 2 video window at 0x2000–0x3FFF uses Slapstic 110 direct, alternate, and bitwise selection. It starts on view 3. Views 0, 2, and 3 have separate backing storage. View 1 reads high and ignores writes. The compiler emits observations for instruction and extension fetches and supported data cycles in bus order. Synthetic tests check the selector sequences and run native reads and writes against the mapped pages. Connecting those pages to rendering remains pending. [MAME Atari System 2 board driver](https://github.com/mamedev/mame/blob/master/src/mame/atari/atarisy2.cpp) · [MAME Slapstic reference](https://github.com/mamedev/mame/blob/master/src/mame/atari/slapstic.cpp)

The palette has 256 read/write words at 0x1000–0x11FF with a mirror at 0x1200–0x13FF. Each word carries four bits per color channel and a shared four-bit intensity. The RGB decoder applies the board reference’s weighted intensity response. Synthetic tests cover the first and last entries, mirror aliasing, and byte writes that preserve the other byte. The palette is available to native code. Vulkan still needs to use it for game rendering. [MAME System 2 address map](https://github.com/mamedev/mame/blob/master/src/mame/atari/atarisy2.cpp) · [MAME System 2 palette reference](https://github.com/mamedev/mame/blob/master/src/mame/atari/atarisy2_v.cpp)

A declared ROM jump table gives the compiler a finite set of targets for an indexed word add to the PC. It resolves the table in the current bank context, checks for overlap with reachable instruction words, and emits native branches. Execution checks the aligned effective table address before reading it and updates flags using the indexed word and the PC after the extension fetch. [MAME DEC T‑11 operand reference](https://github.com/mamedev/mame/blob/master/src/devices/cpu/t11/t11ops.hxx)

A register-driven bank write dispatches to a precompiled path using the board's six-bit selector. The compiler considers all 64 values and preserves the active bank in the other window. Native tests exercise every selector, including a write whose extension word comes from the old bank and whose next instruction comes from the new one. [MAME Atari System 2 bank selector](https://github.com/mamedev/mame/blob/master/src/mame/atari/atarisy2.cpp)

The read-only APB scan starts at T‑11 reset PC 0x8000. It resolves four targets at the startup jump at 0xF15E and passes the register-sourced bank write to 0x1402 at PC 0xF2C4. The scan analyzes 144 reachable PC and bank combinations before stopping at MOVB at PC 0xF2D4.

[GBTGhoulies-Qilincomp design targets](https://github.com/Tecboy1066/GBTGhoulies-Qilincomp/blob/main/docs/roadmap-and-design-targets.md) · [MAME Atari System 2 reference](https://github.com/mamedev/mame/blob/master/src/mame/atari/atarisy2.cpp)
