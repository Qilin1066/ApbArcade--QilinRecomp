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

[GBTGhoulies-Qilincomp design targets](https://github.com/Tecboy1066/GBTGhoulies-Qilincomp/blob/main/docs/roadmap-and-design-targets.md) · [MAME Atari System 2 reference](https://github.com/mamedev/mame/blob/master/src/mame/atari/atarisy2.cpp)
