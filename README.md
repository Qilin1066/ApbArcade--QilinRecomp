# ApbArcade--QilinRecomp

A static recompilation of **APB — All Points Bulletin** (Atari Games, 1987, Atari System 2 arcade hardware) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. It follows the requirements and scope of [GBTGhoulies--QilinRecomp](https://github.com/Qilin1066/GBTGhoulies--QilinRecomp): efficiency first, with a Raspberry Pi 4–class board as the reference for typical low-end hardware, not a floor; anything with a Vulkan 1.3 driver is in scope, Macs included through MoltenVK.

> **No game data is included, and none ever will be.** You need your own copy of the rev 7 ROM set (MAME's `apb`). The tools read it from a directory you give them. `.gitignore` keeps ROMs, and anything generated from them, out of the repo.

## Status

The recompiled game runs; current work focuses on controls, device validation and reliable play at original settings. Real wheel hardware and end-to-end completion remain to be verified.

| Piece | State |
|---|---|
| ROM set: all 36 rev 7 files, MAME's load layout for every region | ✅ size and SHA-1 checked (`qilin verify`) |
| Reference CPU: MAME's T-11 core, vendored unmodified behind a traced bus | ✅ |
| T-11 decoder (full instruction set, cycle counts, MAME-format disassembly) | ✅ agrees with MAME on all 65,536 opcode words: text, length, cycles and traps |
| Board models: memory map, ROM banks, Slapstic 110, palette, interrupt latches, watchdog, sound latches | ✅ each checked against MAME's definitions |
| Renderer: Vulkan 1.3 (shared with GBTGhoulies--QilinRecomp) | ✅ clears, flips and read-back, tested in CI on Mesa's software driver with validation |
| Control flow keyed by physical ROM address (`qilin cfg`): bank tracking, value-range jump tables, structural function discovery | ✅ 27,947 instructions in 805 functions, found with no seeds, across the fixed ROM and all 12 code banks: 78% of the fixed ROM and 66% of the code banks are code |
| Lifter (`qilin lift`): one C++ function per guest function, registers and flags in locals, MAME's bus order and cycle counts, native calls with checked returns | ✅ all 805 functions (30,209 instructions, shared code counted per function) |
| Differential test against MAME's T-11 core: registers, flags, cycles and every bus access in order | ✅ game functions: 6,440 trials, 5,018 matching runs, 0 mismatches; every instruction form (in CI): 148,992 trials, 146,125 matching runs, 0 mismatches |
| Sound CPU: MAME's 6502 as the reference, a 6502 decoder | ✅ agrees with MAME on all 256 opcodes: text, length, cycles, page penalties, flow |
| Sound program control flow (`qilin cfg --cpu sound`): exact RTS jump tables, infeasible branches pruned | ✅ 4,742 instructions in 73 functions (the three vectors, then code nothing else reaches); every table exact |
| Sound lifter (`qilin lift --cpu sound`): MAME's microcode as the instruction bodies; yields and resumes mid-function | ✅ all 73 functions; difftest 4,672 trials, 3,589 matching runs, 0 mismatches; every instruction form (in CI): 23,712 trials, 0 mismatches |
| ROM coverage (`qilin coverage`): every byte code, table, data, fill or unknown | ✅ with no seeds: T-11 code areas and 6502 ROM 100% explained, no unknown byte; every location five reference runs execute is lifted |
| The board scheduler, a reference run of both CPUs (`qilin_refrun`), the game run natively (`qilin_run`) | ✅ attract mode, a credited game, 24 random games of an hour each (216,000 frames), 16 more of 60,000 frames and 16 self-test sessions, no unlifted code; a credited game at about 62× real time on a desktop core; the same frames on ARM64 (cross-built for the Pi 4, run under qemu) |
| Video: the CPU reference renderer (playfield, motion objects, alphanumerics, MAME's priority rule and partial updates) | ✅ boot screens, high scores and gameplay render; `--frame-out FRAME:FILE` saves a frame as a PNG |
| Video through Vulkan: CPU frames uploaded and shown in a window: whole multiples, fit or fill, sharp or smooth; full screen on any display | ✅ tested on Mesa's software driver with validation, under a virtual X server |
| The `apb` executable: window and input on SDL3, the EEPROM saved | ✅ built against SDL3 3.4.20 (fetched) and run on Linux in a virtual X display through Mesa's software Vulkan: attract mode, the settings menu (F1), the EEPROM and settings saved; not yet played with real controls |
| Native and MAME's cores agree frame for frame (`--frame-hashes`) | ✅ attract mode, a credited game, self-test, and random games of up to an hour (216,000 frames): every frame identical |
| Sound: the YM2151 (ymfm, as MAME), both POKEYs (a port of MAME's), MAME's mixer; `qilin_run --wav`, and SDL3 audio in `apb` | ✅ music on coin-up and in the game |
| Speech: the TMS5220C (MAME's, adapted) on the board, its READY line read by the 6502 | ✅ in a credited game the chip speaks (about 5 s in 66 s of play); not yet listened to |
| Controls: any wheel (direct-drive or with a lock), pedals, gamepads, mice and spinners; a settings menu (Dear ImGui) | ✅ tested on SDL's virtual wheel and pedals (`qilin_input_tests`, in CI); not yet tried on real ones |

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
build/qilin coverage "D:/APB Arcade Project" [--seeds S] [--reads R] [--list] [--check-seeds F]   # every ROM byte: code, table, data, fill or unknown (100% explained)
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

### Gameplay regression checks with your ROMs

After lifting both CPUs, enable the optional native/reference tests with your local ROM folder:

```sh
cmake -S . -B build -DQILIN_GEN_DIR="$PWD/gen" -DQILIN_SOUND_GEN_DIR="$PWD/gen-sound" -DQILIN_ROM_DIR="/path/to/APB ROMs"
cmake --build build --target qilin_run qilin_refrun --config Release --parallel
ctest --test-dir build -C Release -L parity --output-on-failure
```

The four picture tests each compare 6,000 frames (about 100 seconds of game time) between the lifted game and MAME's CPU cores: attract mode, a credited game with the pedal and steering held, a repeatable random player, and the self-test menus. It fails on a runner error, missing or incomplete output, or the first differing frame. Logs and hashes stay under `build/game-parity/`. Both runs use this project's board and CPU video renderer; this checks CPU recompilation and scheduling, and does not independently validate the board, sound or Vulkan output against MAME itself. No window or Vulkan driver is needed. A fifth test records a 6,000-frame credited game with synchronous and threaded mixing and requires byte-identical WAVs, including speech (`build/audio-parity/`). Leave `QILIN_ROM_DIR` empty for the usual tests without ROMs.

A separate [standalone MAME comparison](docs/independent-mame.md) checks independently captured pictures and coin audio against MAME 0.264. It reports exact pixel differences, waveform correlation and sound levels, with captures kept locally. With MAME's own turn-taking between the two processors (`--mame-scheduling`, a diagnostic mode of `qilin_run`) the comparison passes: identical pictures and sound commands, audio correlation 0.9999. With the player's own, finer turn-taking the two runs drift apart in timing, as two real boards would; that is by design and is what `apb` uses.

### Playing it

**On Windows, the download (nothing to install).** Each release has `ApbArcade--QilinRecomp-<version>-windows-x64.zip` (about 80 MB; GitHub builds it, `.github/workflows/player-kit.yml`). Unpack it and double-click **`Play APB.bat`**: the first time it asks for your ROM folder and builds the game from it, a couple of minutes, with the compiler inside the download (LLVM-MinGW: Clang, free and open source); after that it starts the game at once. The download has everything but the game's own code, prebuilt: the recompiler, the runtime, SDL3, the menu. The code made from the ROMs is made on the player's PC, from their own copy, and never distributed. See `tools/player-kit/` (`package.sh` builds the download on Linux; `play.ps1` is what Play runs).

**On Windows, from the source, double-click.** With Visual Studio 2022 (17.10 or newer, the free Community edition, with "Desktop development with C++") and Git installed, in the project's folder:

- **`Play APB.bat`** starts the game. The first time it builds it (15 to 40 minutes), asking for your ROM folder with a folder picker.
- **`Update APB.bat`** gets the latest code, rebuilds only what changed, and starts the game.

Both find Visual Studio's CMake and Git themselves; nothing needs typing. If something stops, the window stays open with the reason in red.

**On Linux or macOS:** `tools/build-apb.sh "/path/to/APB ROMs" --run`. (The same script as the .bat files run is `tools/build-apb.ps1 [-RomDir <folder>] [-Run] [-Pull]`.)

Why no ready-made `apb.exe` to download: the game's code in it is made from the ROMs, so it's built on your computer from your own copy and never distributed.

`apb` remembers the ROM folder: after the first run it starts with no arguments (on Windows, double-click `build-apb\Release\apb.exe`). With no folder saved, or one that no longer holds a verified set, it asks with a folder picker. Controllers it finds, connects or loses are shown for a few seconds in the window's corner, and printed to the console.

**By hand:** lift both CPUs and build with the lifted code; `apb` needs SDL3, which CMake uses if it's installed and otherwise fetches (release 3.4.20, linked statically; `-DQILIN_FETCH_SDL3=OFF` to never fetch). Rev 7 needs no seeds: the analysis finds everything the game runs. (Seeds, `--seeds`, are still read: the entries only a run of the game can find, from `qilin_refrun --seeds-out`, for a ROM the analysis doesn't fully cover.)

```sh
build/qilin lift "D:/APB Arcade Project" --out gen
build/qilin lift "D:/APB Arcade Project" --cpu sound --out gen-sound
cmake -S . -B build -DQILIN_GEN_DIR="$PWD/gen" -DQILIN_SOUND_GEN_DIR="$PWD/gen-sound" -DCMAKE_BUILD_TYPE=Release
cmake --build build --target apb qilin_run
build/apb ["D:/APB Arcade Project"] [--scale N] [--no-vsync] [--gpu N] [--mute]
```

| Cabinet | Keyboard | Gamepad |
|---|---|---|
| Steering wheel | Left / Right | left stick, or d-pad left / right |
| Gas pedal | Up | right trigger, or d-pad up |
| Siren / start | Left Ctrl or Z; Space or C also work | A or Start; X also works |
| Fire | Left Alt or X | B |
| Coin 1 / coin 2 / service coin | 5 / 6 / 9 | Back (coin 1) |
| Self-test switch | F2 (toggles) | |
| Settings menu (pauses the game) | F1 or Escape | Back + Start (Start alone closes it) |
| Frame times (on/off) | F3 | |
| Full screen (on/off) | F11 or Alt+Enter | |

The settings menu is laid out as in the other QilinRecomp projects (Punch-Out!!, Beat the Beat, Grabbed by the Ghoulies): Close and Quit at the top right, then Controls, Display and Game pages, with *Look at the picture* and an About section. Pressing Back with Start doesn't drop a coin in: Back counts as a coin once it's been held for three frames (50 ms) without Start.

**Wheels, pedals, spinners.** The settings menu sets them up; nothing is needed for the keyboard or a gamepad. APB's cabinet wheel spins freely and the game counts how far it turns, so a PC wheel drives it by how far its angle moves each frame:

Choose **Free rotation / direct drive** or **Limited rotation / belt or gear driven**. Limited rotation shows a numeric **Maximum angle (degrees)** field: the total range, so 900° means 450° each way. A direct-drive base configured with stops can use Limited too. Input-source details and measurement are under **Advanced wheel input**.

- **MOZA R5:** the *MOZA R5 (free rotation)* setup reads physical degrees through the optional Windows x64 MOZA SDK 1.0.1.8 DLL, independently of a joystick axis that may clip. It needs the user's DLL and the SDK-enabled Pit House build specified by MOZA; no proprietary SDK is bundled. Check the physical-angle meter through several turns. Actual R5 hardware remains unverified.
- **Logitech G920:** *Find it*, then *Logitech G920 (900 degrees)*, matching 900° in G HUB. Held-at-lock steering lets you keep turning the cabinet dial when the wheel reaches a stop.
- **Continuous wrapping axes:** select that input type and set/measure degrees per axis cycle. Every SDL motion event is counted, including seam crossings and complete turns between game frames. This mode cannot recover movement from an axis that clips.
- **Force feedback:** SDL haptics is initialised explicitly, with passive friction/damping and autocentre disabled where supported. The menu reports whether the effect actually started; driver-side centring may need turning off separately.
- **Pedals** (often a separate USB device): *Set up a pedal*, press it to the floor and let it go, *Done*. Pedals that rest at either end of their axis both work.
- **Buttons**: any button, key, hat or axis on any device can be added to each cabinet button and coin.
- **Mouse or spinner**: steers by how far it moves.

See [wheel setup and verification](docs/wheels.md) for the two wheel setups, SDK installation, continuous rotation, calibration and what still needs testing on hardware.

**Display.** The menu's Display page: presets (*Original*, *Arcade monitor*, *Smooth*), full screen, which display, the picture's size (whole multiples, the sharpest; fit, keeping its shape; or filling the window), its look (plain pixels, smooth or sharp; smoothed pixel art; or a CRT, with scanlines running down the screen as on the cabinet's upright monitor), vsync, the speed (exact: the cabinet's 60.1 Hz, the newest frame at each refresh; or matched to a display near 60 Hz or a multiple, every frame shown in turn, the game a fraction of a percent slow), and which graphics device (used from the next start). `--no-vsync` and `--gpu N` override the saved choices for one run. The game always runs at the cabinet's 60.1 frames a second.

Each configured wheel has a saved control profile: its rotation mode, angle, calibration, pedals, buttons and friction. Choose **Saved wheel setup** in Controls, or reconnect a previously configured wheel while the selected one is absent. Bind each wheel once; profiles use SDL's device GUID, so two identical units with the same GUID share a setup. Display settings and the ROM folder are shared.

Alt-Tab or minimising pauses gameplay and audio (the window says *Paused: click the window to carry on*), suspends game wheel effects, and rebases steering on return. Audio-device outages retry automatically without restarting APB; playback resumes from current game audio.

Settings are saved to `apb.ini`, and the EEPROM (high scores, audits, the game's own settings) to `eeprom.bin`, both in the game's data folder, `qilin/apb` in your per-user data as the other QilinRecomp projects keep theirs (`%LOCALAPPDATA%\qilin\apb` on Windows, `~/Library/Application Support/qilin/apb` on macOS, `$XDG_DATA_HOME` or `~/.local/share/qilin/apb` on Linux; the Game page shows it). Up to v0.21.0 it was SDL's preferences folder for `Qilin/apb` (`%APPDATA%\Qilin\apb` on Windows): the first run moves what's there across. Settings and EEPROM use flushed temporary files and atomic replacement. Changed EEPROM is saved every five seconds, when opening the pause menu, and on exit; `eeprom.bin.bak` holds the previous complete save. An incomplete primary is recovered from that backup with an on-screen notice. Storage failures are reported. Abrupt loss of power can still lose the most recent unsaved changes.

WAV exports convert the fractional chip rate (55,930.39453125 Hz) to the file's integer 55,930 Hz rate, preserving the timebase. The independent MAME check also replays its bus trace through the native sound chips to distinguish chip/mixer errors from CPU scheduling differences.

Sound mixing runs on a worker with ordered chip events and bounded buffering. Playback prebuffers two frames, refills after starvation, clears queued sound on pause, and smoothly adjusts playback rate by at most 2% to follow device/display clock drift. It no longer discards whole frames of sound when the playback queue fills. These changes preserve the chip mix: `qilin_run --wav FILE --audio-threaded 1` exercises the same worker for offline comparison.

For an actual R5/G920 and display check, the Windows download includes **Check controls and performance.bat**. It records a session and collects controller/performance reports and a checklist in a new `Hardware checks` folder. Play, quit normally and fill in the physical observations; see [actual-machine capture](docs/profiling.md#actual-machine-capture). Source builds can use `apb --controller-report FILE` to export automatically at exit.

The player and headless runner support `--record session.qir` and `--replay session.qir` for reproducible cabinet inputs, with initial EEPROM and checksums. Replay preserves personal high-score saves. See [input recordings](docs/input-movies.md). Use `--profile performance.txt` to measure simulation, audio and frame pacing; see [profiling](docs/profiling.md). Controls → **Save controller diagnostics** exports device ranges and the MOZA angle status for hardware testing; see [wheel setup](docs/wheels.md#saving-a-controller-report).

The independent MAME harness now replays captured per-band graphics state through `qilin_video_trace`: all five sampled pictures match exactly when given the same state. Run independently with the player's own scheduler, the complete games differ from MAME in timing; with MAME's scheduler they match. See [the measured checks and limitations](docs/independent-mame.md).

`qilin_run` runs the same game headless with scripted inputs; `--frame-out FRAME:FILE` saves frames as PNGs (`--look crt:1152x1536` as a window that size would show them, through a look), `--draw 1` draws every frame to measure what video costs, and `--wav FILE` records the sound. `qilin_tween` (a prototype, needs ffmpeg) renders a stretch of a run with frames drawn between the game's, beside the game as it is, to a video.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `romset` (the rev 7 set), `t11_decoder`, `cfg` (control flow), `vrange` (value ranges), `lifter` (T-11 → C++); `m6502_decoder`, `m6502_cfg`, `m6502_lifter` (the sound CPU); `main` (the `qilin` CLI) |
| `recomp/runtime/` | Runtime headers the generated code includes: `t11_rt.hpp` and `m6502_rt.hpp` (CPU state, dispatch, the board contracts), `system2.hpp` (the board as the T-11 sees it) |
| `recomp/runtime/gfx/` | The renderer: Vulkan 1.3, loaded at run time |
| `recomp/tests/` | Unit tests; they need no game files |
| `tools/check/` | Checks against MAME: both decoders on every opcode; both lifters on every instruction form |
| `tools/difftest/`, `tools/difftest6502/` | Differential tests: lifted game functions against MAME's T-11 and 6502 cores |
| `third_party/` | Vendored: the Vulkan headers and volk (as in GBTGhoulies--QilinRecomp), and MAME's T-11 and 6502 cores (the reference CPUs). See `third_party/README.md` |
| `docs/` | Design targets, the board, the recompiler's status, naming |

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0), as in the other QilinRecomp projects: you may use, share, change and sell it, but anything you distribute that is built from it must come with its full source under the same licence. (Versions before 2026-11-21 were released under MIT, and those copies stay under MIT.) Some things in or around it are not:

- The vendored code in `third_party/` keeps its own licences: MAME's T-11, 6502 and TMS5220 cores and ymfm (BSD-3-Clause), Dear ImGui (MIT), volk (MIT) and the Vulkan headers (Apache-2.0 or MIT). The MAME scheduler port in `recomp/runtime/mame/` follows MAME's BSD-3-Clause. See `third_party/README.md`.
- SDL3 (zlib), fetched by CMake at build time, and in the Windows download LLVM-MinGW and Ninja (Apache-2.0), keep theirs.

The game itself is Atari Games' and is not covered by anything here: you need your own ROMs, and nothing built from them may be shared.

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
