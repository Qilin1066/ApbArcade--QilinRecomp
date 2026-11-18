# ApbArcade-QilinComp

A static recompilation of **APB — All Points Bulletin** (Atari Games, 1987, Atari System 2 arcade hardware) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. It follows the requirements and scope of [GBTGhoulies-Qilincomp](https://github.com/Tecboy1066/GBTGhoulies-Qilincomp): efficiency first, with a Raspberry Pi 4–class board as the reference for typical low-end hardware, not a floor; anything with a Vulkan 1.3 driver is in scope, Macs included through MoltenVK.

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
| Coverage of the rest of the ROM, the board scheduler, `apb` booting to its first frame | 🔨 next |
| 6502 sound program, sound chips, video, input | to do |

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
```

### Differential test against your copy of the game

```sh
build/qilin lift "D:/APB Arcade Project" --out gen
cmake -S . -B build -DQILIN_GEN_DIR="$PWD/gen"
cmake --build build --target qilin_difftest
build/qilin_difftest "D:/APB Arcade Project" [--trials N] [--seed S] [--only 8288]
```

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `romset` (the rev 7 set), `t11_decoder`, `cfg` (control flow), `vrange` (value ranges), `lifter` (T-11 → C++), `main` (the `qilin` CLI) |
| `recomp/runtime/` | Runtime headers the generated code includes: `t11_rt.hpp` (CPU state, dispatch, the board contract), `system2.hpp` (the board as the T-11 sees it) |
| `recomp/runtime/gfx/` | The renderer: Vulkan 1.3, loaded at run time |
| `recomp/tests/` | Unit tests; they need no game files |
| `tools/check/` | Checks against MAME: the decoder on every opcode word; the lifter on every instruction form |
| `tools/difftest/` | Differential test: lifted game functions against MAME's T-11 core |
| `third_party/` | Vendored: the Vulkan headers and volk (as in GBTGhoulies-Qilincomp) and MAME's T-11 core (the reference CPU). See `third_party/README.md` |
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
