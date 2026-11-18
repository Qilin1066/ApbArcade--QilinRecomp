# ApbArcade-QilinComp

A static recompilation of **APB — All Points Bulletin** (Atari Games, 1987, Atari System 2 arcade hardware) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. It follows the requirements and scope of [GBTGhoulies-Qilincomp](https://github.com/Qilin1066/GBTGhoulies-Qilincomp): efficiency first, with a Raspberry Pi 4–class board as the reference for typical low-end hardware, not a floor; anything with a Vulkan 1.3 driver is in scope, Macs included through MoltenVK.

> **No game data is included, and none ever will be.** You need your own copy of the rev 7 ROM set (MAME's `apb`). The tools read it from a directory you give them. `.gitignore` keeps ROMs, and anything generated from them, out of the repo.

## Status

Roadmap step 2, **CPU coverage**, is in progress.

| Piece | State |
|---|---|
| ROM set: all 36 rev 7 files, MAME's load layout for every region | ✅ size and SHA-1 checked (`qilin verify`) |
| Reference CPU: MAME's T-11 core, vendored unmodified behind a traced bus | ✅ |
| T-11 decoder (full instruction set, cycle counts, MAME-format disassembly) | ✅ agrees with MAME on all 65,536 opcode words: text, length, cycles and traps |
| Board models: memory map, ROM banks, Slapstic 110, palette, interrupt latches, watchdog, sound latches | ✅ each checked against MAME's definitions |
| Renderer: Vulkan 1.3 (shared with GBTGhoulies-Qilincomp) | ✅ clears, flips and read-back, tested in CI on Mesa's software driver with validation |
| Control flow keyed by physical ROM address (`qilin cfg`): bank tracking, value-range jump tables, structural function discovery | ✅ 24,063 instructions in 659 functions reached from reset, across the fixed ROM and all 12 code banks; 53% of the fixed ROM and 62% of the code banks proven code |
| Lifter (`qilin lift`): one C++ function per guest function, registers and flags in locals, MAME's bus order and cycle counts, native calls with checked returns | ✅ all 659 functions, 25,554 instructions |
| Differential test against MAME's T-11 core: registers, flags, cycles and every bus access in order | ✅ game functions: 32,698 trials, 0 mismatches; every instruction form (in CI): 145,606 trials, 0 mismatches |
| Sound CPU: MAME's 6502 as the reference, a 6502 decoder | ✅ agrees with MAME on all 256 opcodes: text, length, cycles, page penalties, flow |
| Sound program control flow (`qilin cfg --cpu sound`): exact RTS jump tables, infeasible branches pruned | ✅ 4,618 instructions in 66 functions from the three vectors; every table exact, nothing undocumented |
| Sound lifter (`qilin lift --cpu sound`): MAME's microcode as the instruction bodies; yields and resumes mid-function | ✅ all 66 functions; difftest 3,312 trials, 0 mismatches; every instruction form (in CI): 23,520 trials, 0 mismatches |
| The board scheduler, a reference run of both CPUs (`qilin_refrun`), the game run natively (`qilin_run`) | ✅ attract mode and a credited game, 20,000 frames each, about 60× real time, no unlifted code |
| Video: the CPU reference renderer (playfield, motion objects, alphanumerics, MAME's priority rule and partial updates) | ✅ boot screens, high scores and gameplay render; `--frame-out FRAME:FILE` saves a frame as a PNG |
| Video through Vulkan: CPU frames uploaded and shown in a window, scaled by whole numbers | ✅ tested on Mesa's software driver with validation, under a virtual X server |
| The `apb` executable: window and input on SDL3, the EEPROM saved | 🔨 written, not yet built or played here (SDL3 couldn't be fetched in this environment) |
| Sound chips | to do |

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). Past entries are kept, never rewritten. The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. While the recompiler is being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game.

## Building

The whole project (recompiler, runtime and generated code) is **C++23**. You need CMake ≥ 3.25 and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops with a clear message if the compiler is older. There are no other dependencies: the Vulkan headers and volk are vendored, and Vulkan itself is loaded at run time.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CI builds and tests with warnings as errors on Linux x64 (GCC 14 and Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and x64 (the runtime built for macOS 10.15) and Windows x64 (MSVC). The renderer test skips (exit 77) on a machine without Vulkan.

## Using it

```sh
build/qilin verify "D:/APB Arcade Project"                       # all 36 files, size and SHA-1
build/qilin insns  "D:/APB Arcade Project" --from 8000 --to 8100 # T-11 listing (MAME syntax, cycles)
build/qilin insns  "D:/APB Arcade Project" --banks 3,12 --from 4000 --to 4100
build/qilin cfg    "D:/APB Arcade Project" [--all]               # control flow from reset, and all it can't follow yet
build/qilin lift   "D:/APB Arcade Project" --out gen             # lift to C++ (gen/ is gitignored: never commit it)
build/qilin insns  "D:/APB Arcade Project" --cpu sound --from 4000 --to 4100   # 6502 listing
build/qilin cfg    "D:/APB Arcade Project" --cpu sound [--all]   # the sound program's control flow
build/qilin lift   "D:/APB Arcade Project" --cpu sound --out gen-sound
```

### Differential test against your copy of the game

```sh
build/qilin lift "D:/APB Arcade Project" --out gen
cmake -S . -B build -DQILIN_GEN_DIR="$PWD/gen"
cmake --build build --target qilin_difftest
build/qilin_difftest "D:/APB Arcade Project" [--trials N] [--seed S] [--only 8288]
```

The sound program the same way:

```sh
build/qilin lift "D:/APB Arcade Project" --cpu sound --out gen-sound
cmake -S . -B build -DQILIN_SOUND_GEN_DIR="$PWD/gen-sound"
cmake --build build --target qilin_sound_difftest
build/qilin_sound_difftest "D:/APB Arcade Project" [--trials N] [--seed S] [--only 5066]
```

### Playing it

Seeds first: entries only a run of the game can find (see `docs/recompiler-core.md`). Two reference runs, attract mode and then a credited game, each 20,000 frames, find all 36 that rev 7 needs (about a minute). Then lift both CPUs and build with the lifted code; `apb` needs SDL3, which CMake uses if it's installed and otherwise fetches (release 3.4.20, linked statically; `-DQILIN_FETCH_SDL3=OFF` to never fetch).

```sh
build/qilin_refrun "D:/APB Arcade Project" --frames 20000 --seeds-out attract.seeds
build/qilin_refrun "D:/APB Arcade Project" --frames 20000 --coin 1500 --coin 1560 --press 0008:1700 --pedal 63 --steer 1 --seeds attract.seeds --seeds-out apb.seeds
build/qilin lift "D:/APB Arcade Project" --seeds apb.seeds --out gen
build/qilin lift "D:/APB Arcade Project" --cpu sound --out gen-sound
cmake -S . -B build -DQILIN_GEN_DIR="$PWD/gen" -DQILIN_SOUND_GEN_DIR="$PWD/gen-sound" -DCMAKE_BUILD_TYPE=Release
cmake --build build --target apb qilin_run
build/apb "D:/APB Arcade Project" [--scale N] [--no-vsync] [--gpu N]
```

| Cabinet | Keyboard | Gamepad |
|---|---|---|
| Steering wheel | Left / Right | left stick |
| Gas pedal | Up | right trigger |
| Button 1 / 2 / 3 (MAME's numbering) | Left Ctrl or Z / Left Alt or X / Space or C | A / B / X |
| Coin 1 / coin 2 / service coin | 5 / 6 / 9 | Back (coin 1) |
| Self-test switch | F2 (toggles) | |
| Quit | Escape | |

The EEPROM (high scores, audits, settings) is saved in your user preferences folder (SDL's for `Qilin/apb`). `qilin_run` runs the same game headless with scripted inputs; `--frame-out FRAME:FILE` saves frames as PNGs and `--draw 1` draws every frame to measure what video costs.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `romset` (the rev 7 set), `t11_decoder`, `cfg` (control flow), `vrange` (value ranges), `lifter` (T-11 → C++); `m6502_decoder`, `m6502_cfg`, `m6502_lifter` (the sound CPU); `main` (the `qilin` CLI) |
| `recomp/runtime/` | Runtime headers the generated code includes: `t11_rt.hpp` and `m6502_rt.hpp` (CPU state, dispatch, the board contracts), `system2.hpp` (the board as the T-11 sees it) |
| `recomp/runtime/gfx/` | The renderer: Vulkan 1.3, loaded at run time |
| `recomp/tests/` | Unit tests; they need no game files |
| `tools/check/` | Checks against MAME: both decoders on every opcode; both lifters on every instruction form |
| `tools/difftest/`, `tools/difftest6502/` | Differential tests: lifted game functions against MAME's T-11 and 6502 cores |
| `third_party/` | Vendored: the Vulkan headers and volk (as in GBTGhoulies-Qilincomp), and MAME's T-11 and 6502 cores (the reference CPUs). See `third_party/README.md` |
| `docs/` | Design targets, the board, the recompiler's status, naming |

## Design targets (short version)

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, T-11 and 6502 → C++, compiled ahead of time.** No interpreter and no JIT. Hand-written C++ can override any lifted function.
- **Code is identified by its physical ROM address**, so banked code exists once per bank, and bank registers are runtime state.
- **Vulkan 1.3 and only Vulkan**, with the feature set of a Pi 4's v3dv driver, so one render path runs on everything. macOS runs through MoltenVK.
- **Ground truth is MAME** running the original ROMs; its T-11 core is the reference every check runs against.

See `docs/roadmap-and-design-targets.md` for the full design and roadmap, and `docs/system2.md` for the hardware.

## References

- Board, memory map, ROM layout and devices: MAME's Atari System 2 driver ([atarisy2.cpp](https://github.com/mamedev/mame/blob/master/src/mame/atari/atarisy2.cpp), [slapstic.cpp](https://github.com/mamedev/mame/blob/master/src/mame/atari/slapstic.cpp)).
- T-11: MAME's core ([src/devices/cpu/t11](https://github.com/mamedev/mame/tree/master/src/devices/cpu/t11)) and DEC's [DCT11 User's Guide](https://www.bitsavers.org/pdf/dec/pdp11/t11/T11_UsersMan.pdf).
- Vulkan drivers: [Mesa 24.3](https://docs.mesa3d.org/relnotes/24.3.0.html), [MoltenVK](https://github.com/KhronosGroup/MoltenVK).
