# ApbArcade--QilinRecomp

A static recompilation of **APB — All Points Bulletin** (Atari Games, Atari System 2 arcade hardware, 1987) into portable C++23 with a Vulkan 1.3 renderer, for 64-bit Windows, Linux and macOS on x86-64 or ARM64. Any GPU with a Vulkan 1.3 driver is in scope, down to the Raspberry Pi 4 (Mesa 24.3+). Macs run it through MoltenVK (macOS 10.15 or later).

> **No game data is included, and none ever will be.** You need your own copy of the rev 7 ROM set (MAME's `apb`). The tools read it from a directory you give them.

## Status

The recompiled game runs from attract mode through credited games, with music, speech, saved high scores and a settings menu, and matches MAME's CPU cores frame for frame. It has not yet been tried on real wheels and pedals or played to the end.

| Part | State |
|---|---|
| ROM set (36 rev 7 files) | ✅ size and SHA-1 checked (`qilin verify`) |
| T-11 decoder | ✅ agrees with MAME on all 65,536 opcode words: text, length, cycles, traps |
| Board models (memory map, ROM banks, Slapstic 110, palette, interrupts, watchdog, sound latches) | ✅ checked against MAME's definitions |
| Main CPU control flow (`qilin cfg`) | ✅ 27,947 instructions in 805 functions, no seeds; 78% of the fixed ROM and 66% of the 12 code banks are code |
| Main CPU lifter (`qilin lift`) | ✅ all 805 functions; differential test 0 mismatches |
| 6502 sound CPU: decoder, control flow, lifter | ✅ decoder agrees with MAME on all 256 opcodes; 4,742 instructions in 73 functions, all lifted; 0 mismatches |
| ROM coverage (`qilin coverage`) | ✅ T-11 code areas and the 6502 ROM 100% explained, no unknown byte |
| Native game against MAME's cores (`--frame-hashes`) | ✅ every frame identical in attract mode, credited games, self-test and random games of up to an hour |
| Long runs | ✅ 24 random games of an hour each with no unlifted code; about 62× real time on a desktop core; same frames on ARM64 under qemu |
| Video (CPU renderer, shown through Vulkan) | ✅ boot screens, high scores and gameplay; tested on Mesa's software driver with validation |
| Sound: YM2151 (ymfm), two POKEYs, MAME's mixer | ✅ music on coin-up and in the game |
| Speech: TMS5220C | 🔨 the chip speaks in a credited game; not yet listened to |
| `apb` player (SDL3 window, input, audio, EEPROM, Dear ImGui menu) | ✅ runs on Linux under a virtual X display; not yet played with real controls |
| Controls: wheels, pedals, gamepads, mice, spinners | 🔨 tested on SDL's virtual wheel and pedals (`qilin_input_tests`, in CI); real MOZA R5 and Logitech G920 unverified |

The latest validation record is [`docs/reliability-0.23.md`](docs/reliability-0.23.md).

## Playing

### Windows download

Each release has `ApbArcade--QilinRecomp-<version>-windows-x64.zip`, built by GitHub (`.github/workflows/player-kit.yml`). Unpack it and double-click **`Play APB.bat`**. The first time, it asks for your ROM folder and builds the game from it in a couple of minutes, with the compiler included in the download (LLVM-MinGW). After that it starts the game at once.

The download holds everything prebuilt except the game's own code, which is made on your PC from your own ROMs and never distributed; that is why there is no ready-made `apb.exe`. The kit is built from `tools/player-kit/`.

### From the source on Windows

With Visual Studio 2022 (17.10 or newer, with "Desktop development with C++") and Git installed, in the project's folder:

- **`Play APB.bat`** starts the game. The first time it builds it (15 to 40 minutes), asking for your ROM folder with a folder picker.
- **`Update APB.bat`** gets the latest code, rebuilds only what changed, and starts the game.

Both find Visual Studio's CMake and Git themselves. If something stops, the window stays open with the reason in red. They run `tools/build-apb.ps1 [-RomDir <folder>] [-Run] [-Pull]`.

### Linux and macOS

```sh
tools/build-apb.sh "/path/to/APB ROMs" --run
```

`apb` remembers the ROM folder, so after the first run it starts with no arguments (on Windows, `build-apb\Release\apb.exe`). Without a verified folder it asks with a folder picker.

### Controls

| Cabinet | Keyboard | Gamepad |
|---|---|---|
| Steering wheel | Left / Right | left stick, or d-pad left / right |
| Gas pedal | Up | right trigger, or d-pad up |
| Siren / start | Left Ctrl or Z; Space or C also work | A or Start; X also works |
| Fire | Left Alt or X | B |
| Coin 1 / coin 2 / service coin | 5 / 6 / 9 | Back (coin 1) |
| Self-test switch | F2 (toggles) | |
| Settings menu (pauses the game) | F1 or Escape | Back + Start (Start alone closes it) |
| Menu pages | Page Up / Page Down | LB / RB |
| Frame times (on/off) | F3 | |
| Full screen (on/off) | F11 or Alt+Enter | |

The settings menu has Controls, Display and Game pages. Back counts as a coin only once held for 50 ms without Start, so Back + Start doesn't drop a coin in. Switching away from the window pauses the game and its sound.

### Wheels, pedals and spinners

The keyboard and gamepads need no setting up; the settings menu sets up everything else. APB's cabinet wheel spins freely, so a PC wheel drives it by how far its angle moves each frame. Choose **Free rotation / direct drive** or **Limited rotation / belt or gear driven** (with a **Maximum angle (degrees)**, the total range). There are setups for the **MOZA R5** (physical degrees through your own copy of the optional MOZA SDK 1.0.1.8 DLL on Windows x64; none is bundled) and the **Logitech G920** (900°, as set in G HUB), plus continuous wrapping axes, pedals, any button, key, hat or axis on any device, and a mouse or spinner. Each wheel keeps its own saved profile. See [wheel setup and verification](docs/wheels.md).

### Display

The Display page has presets (*Original*, *Arcade monitor*, *Smooth*), full screen, the picture's size (whole multiples, fit or fill), its look (plain, smoothed pixel art, or a CRT with scanlines running down the screen as on the cabinet's upright monitor), vsync, the graphics device, and the speed: exact (the cabinet's 60.1 Hz) or matched to a display near 60 Hz (every frame shown, a fraction of a percent slow). `--no-vsync` and `--gpu N` override the saved choices for one run.

### Settings and saves

Settings go in `apb.ini` and the EEPROM (high scores, audits, the game's own settings) in `eeprom.bin`, in `%LOCALAPPDATA%\qilin\apb` on Windows, `~/Library/Application Support/qilin/apb` on macOS, and `$XDG_DATA_HOME` or `~/.local/share/qilin/apb` on Linux. Files in the older `%APPDATA%\Qilin\apb` are moved across on the first run. Saves are atomic; the EEPROM is saved every five seconds, on opening the menu and on exit, with the previous save kept as `eeprom.bin.bak` and used if the primary is incomplete.

### Checking real hardware

The Windows download includes **`Check controls and performance.bat`**, which records a session and collects controller and performance reports and a checklist in a `Hardware checks` folder ([actual-machine capture](docs/profiling.md#actual-machine-capture)). From the source, `apb --controller-report FILE` writes the report at exit.

## Building

The whole project (recompiler, runtime and generated code) is C++23. You need CMake 3.25 or newer and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops with a clear message if the compiler is older. The Vulkan headers and volk are vendored, and Vulkan is loaded at run time. `apb` needs SDL3: CMake uses an installed one, or else fetches release 3.4.20 and links it statically (`-DQILIN_FETCH_SDL3=OFF` never fetches).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Without lifted code this builds the recompiler, the tests, and the player linked against empty fixtures (a build check, not a playable game). To build the game by hand, lift both CPUs and point CMake at the output:

```sh
build/qilin lift /path/to/roms --out gen
build/qilin lift /path/to/roms --cpu sound --out gen-sound
cmake -S . -B build -DQILIN_GEN_DIR="$PWD/gen" -DQILIN_SOUND_GEN_DIR="$PWD/gen-sound" -DCMAKE_BUILD_TYPE=Release
cmake --build build --target apb qilin_run
build/apb [/path/to/roms] [--scale N] [--no-vsync] [--gpu N] [--mute]
```

`gen/` and `gen-sound/` are gitignored: never commit them. Rev 7 needs no seeds. `--seeds FILE` is still read, for a ROM the analysis doesn't fully cover, with entries from `qilin_refrun --seeds-out`.

CI builds and tests with warnings as errors on Linux x64 (GCC 14 and Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and x64 (the runtime built for macOS 10.15) and Windows x64 (MSVC). The renderer test skips (exit 77) on a machine without Vulkan, but Linux CI requires it to run.

## Using it

```sh
build/qilin --version
build/qilin verify   /path/to/roms                                   # all 36 files, size and SHA-1
build/qilin insns    /path/to/roms --from 8000 --to 8100             # T-11 listing (MAME syntax, cycles)
build/qilin insns    /path/to/roms --banks 3,12 --from 4000 --to 4100
build/qilin cfg      /path/to/roms [--all]                           # control flow from reset, and what it can't follow
build/qilin coverage /path/to/roms [--seeds S] [--reads R] [--list] [--check-seeds F]   # every ROM byte: code, table, data, fill or unknown
build/qilin lift     /path/to/roms --out gen                         # lift to C++
build/qilin insns    /path/to/roms --cpu sound --from 4000 --to 4100 # 6502 listing
build/qilin cfg      /path/to/roms --cpu sound [--all]               # the sound program's control flow
build/qilin lift     /path/to/roms --cpu sound --out gen-sound
```

`qilin_run` runs the game headless with scripted inputs (`--coin`, `--press`, `--pedal`, `--steer`, `--monkey`, `--test-switch`). `--frame-out FRAME:FILE` saves a frame as a PNG (`--look crt:1152x1536` renders it as a window that size would show it, through a look), `--draw 1` draws every frame to measure what video costs, `--wav FILE` records the sound, and `--frame-hashes FILE` writes a hash per frame. Both `qilin_run` and `apb` take `--record FILE` and `--replay FILE` for repeatable inputs ([input recordings](docs/input-movies.md)) and `--profile FILE` to measure frame pacing ([profiling](docs/profiling.md)).

### Differential test

Each lifted function is run against MAME's core from random states, and the registers, flags, cycles and every bus access must match in order.

```sh
build/qilin lift /path/to/roms --out gen
cmake -S . -B build -DQILIN_GEN_DIR="$PWD/gen"
cmake --build build --target qilin_difftest
build/qilin_difftest /path/to/roms [--trials N] [--seed S] [--only 8288]
```

The sound program the same way:

```sh
build/qilin lift /path/to/roms --cpu sound --out gen-sound
cmake -S . -B build -DQILIN_SOUND_GEN_DIR="$PWD/gen-sound"
cmake --build build --target qilin_sound_difftest
build/qilin_sound_difftest /path/to/roms [--trials N] [--seed S] [--only 5066]
```

Current results: T-11 6,440 trials, 5,018 matching runs, 0 mismatches; 6502 4,672 trials, 3,589 matching runs, 0 mismatches. The remaining trials are inconclusive (they don't return, or reach unlifted code or RAM). Without ROMs, `qilin_lifter_check` and `qilin_sound_lifter_check` run both lifters on every instruction form in CI: 148,992 trials (146,125 matching runs) and 23,712 trials, 0 mismatches.

### Gameplay parity checks

With both CPUs lifted, give CMake your ROM folder to enable the ROM tests:

```sh
cmake -S . -B build -DQILIN_GEN_DIR="$PWD/gen" -DQILIN_SOUND_GEN_DIR="$PWD/gen-sound" -DQILIN_ROM_DIR="/path/to/APB ROMs"
cmake --build build --target qilin_run qilin_refrun --config Release --parallel
ctest --test-dir build -C Release -L parity --output-on-failure
```

- Four picture tests (attract mode, a credited game with the pedal and steering held, a repeatable random player, the self-test menus) each compare 6,000 frames, about 100 seconds, between the lifted game and MAME's CPU cores. Each fails on a runner error, missing output or the first differing frame; logs and hashes are in `build/game-parity/`. Both sides use this project's board and video models, so this proves the CPU recompilation and scheduling, not the board, sound or Vulkan output.
- The audio test records a 6,000-frame credited game with synchronous and threaded mixing and requires byte-identical WAVs, speech included (`build/audio-parity/`).
- The recording test records a 6,000-frame random game and replays it, requiring identical frame hashes and WAVs.

No window or Vulkan driver is needed. Leave `QILIN_ROM_DIR` empty for the tests without ROMs.

### Standalone MAME comparison

[`docs/independent-mame.md`](docs/independent-mame.md) compares independently captured pictures and coin audio with MAME 0.264. With MAME's scheduling of the two CPUs (`qilin_run --mame-scheduling`) the pictures and sound commands are identical and audio correlation is 0.9999. With the finer scheduling `apb` uses, the runs drift apart in timing, as two real boards would.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `romset` (the rev 7 set), `t11_decoder`, `cfg` (control flow), `vrange` (value ranges), `coverage`, `lifter` (T-11 → C++); `m6502_decoder`, `m6502_cfg`, `m6502_lifter` (the sound CPU); `main` (the `qilin` CLI) |
| `recomp/runtime/` | Runtime: `t11_rt.hpp` and `m6502_rt.hpp` (CPU state, dispatch), `system2*` (the board, video, sound and speech), the audio worker, persistence, input recordings, profiling |
| `recomp/runtime/app/` | The `apb` player: input, audio output, the settings menu, MOZA support |
| `recomp/runtime/gfx/` | The renderer: Vulkan 1.3, loaded at run time |
| `recomp/runtime/mame/` | Port of MAME's scheduler, for the reference runs |
| `recomp/tests/` | Unit tests; they need no game files |
| `tools/run/`, `tools/refrun/` | `qilin_run` (the game, headless) and `qilin_refrun` (the same on MAME's cores) |
| `tools/difftest/`, `tools/difftest6502/` | Differential tests: lifted game functions against MAME's T-11 and 6502 cores |
| `tools/check/` | Checks against MAME: both decoders on every opcode, both lifters on every instruction form, the parity tests |
| `tools/handoff/`, `tools/audio_trace/`, `tools/video_trace/` | Diagnostics: hand a native run over to MAME's cores; trace audio and video state |
| `tools/player-kit/` | The Windows download: packaging, the launcher, the hardware check |
| `tools/build-apb.sh`, `tools/build-apb.ps1` | One-step build of the game from your ROMs |
| `third_party/` | Vendored: the Vulkan headers, volk, Dear ImGui, ymfm, and MAME's T-11, 6502 and TMS5220 cores. See `third_party/README.md` |
| `docs/` | Design targets and roadmap, the board, the recompiler, wheels, profiling, validation records, naming |

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. A minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the whole game.

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0): you may use, share, change and sell it, but anything you distribute that is built from it must come with its full source under the same licence. Versions before 2026-11-21 were released under MIT, and those copies stay under MIT.

The vendored code in `third_party/` keeps its own licences: MAME's T-11, 6502 and TMS5220 cores and ymfm (BSD-3-Clause), Dear ImGui (MIT), volk (MIT) and the Vulkan headers (Apache-2.0 or MIT). The MAME scheduler port in `recomp/runtime/mame/` follows MAME's BSD-3-Clause. SDL3 (zlib), fetched at build time, and LLVM-MinGW and Ninja (Apache-2.0) in the Windows download keep theirs. See `third_party/README.md`.

The game itself is Atari Games' and is not covered by anything here: you need your own ROMs, and nothing built from them may be shared.

## Design targets

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, T-11 and 6502 → C++, compiled ahead of time.** No interpreter and no JIT. Hand-written C++ can override any lifted function.
- **Code is identified by its physical ROM address**, so banked code exists once per bank, and bank registers are runtime state.
- **Vulkan 1.3 only**, with the feature set of the Pi 4's v3dv driver, so one render path runs on everything ([Mesa 24.3](https://docs.mesa3d.org/relnotes/24.3.0.html), and [MoltenVK](https://github.com/KhronosGroup/MoltenVK) on macOS).
- **Ground truth is MAME** running the original ROMs: its Atari System 2 driver ([atarisy2.cpp](https://github.com/mamedev/mame/blob/master/src/mame/atari/atarisy2.cpp), [slapstic.cpp](https://github.com/mamedev/mame/blob/master/src/mame/atari/slapstic.cpp)) for the board, and its [T-11 core](https://github.com/mamedev/mame/tree/master/src/devices/cpu/t11), with DEC's [DCT11 User's Guide](https://www.bitsavers.org/pdf/dec/pdp11/t11/T11_UsersMan.pdf), for the CPU every check runs against.

See [`docs/roadmap-and-design-targets.md`](docs/roadmap-and-design-targets.md) for the full design and roadmap, and [`docs/system2.md`](docs/system2.md) for the hardware.
