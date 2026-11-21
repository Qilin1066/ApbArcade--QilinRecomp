# Independent MAME checks

The usual `ctest -L parity` compares lifted CPUs against MAME CPU cores using **shared** board/video models, and compares threaded vs synchronous audio. These tests catch recompilation or threading regressions, but cannot find an error shared by those board or audio models.

`tools/check/mame_compare.py` runs a separate, original MAME 0.264 executable. Its board, graphics, audio, CPU scheduling and input framework are independent of this project's models. The script captures raw RGB32 screen pixels through MAME's Lua API, applies APB's ROT270 orientation, and compares every RGB pixel with native PNG captures. It also records both stereo WAVs, aligns a coin-sound window with one shared bounded stereo lag, and reports waveform correlation, RMS and spectral band level differences. It does not search for a different matching frame for each picture.

Supply MAME 0.264, a native build with both CPUs lifted, the matching `qilin_audio_trace` tool, and your extracted rev 7 ROM directory. The replay tool is built by default beside `qilin_run`; use `--audio-trace /path/to/qilin_audio_trace` if it lives elsewhere. Python dependencies are NumPy, SciPy and Pillow:

```sh
python -m pip install numpy scipy Pillow
python tools/check/mame_compare.py \
  --mame /path/to/mame \
  --native build/qilin_run \
  --rom-dir /path/to/extracted/apb \
  --out build/independent-mame
```

Choose a new output directory on each run: old captures cannot be reused accidentally. The script verifies MAME's version, builds a private local `apb.zip`, uses fresh NVRAM/configuration directories, runs without a window or physical audio device, and retains Lua, logs, both WAVs, pictures, white-on-black mismatch masks and `report.json`. Do not distribute these game-derived captures or ROMs. No captures, ROMs or expected copyrighted images are committed to the repository.

The scenario inserts coins at native frames 900 and 930, presses siren/start at 3100 and 4000, leaves steering stationary and sets the pedal to released ADC value 63 in both engines. MAME inputs are shifted by the explicit boot offset. The default `--boot-offset 4` compares MAME frame N with native N−4, reflecting the previously observed startup lead; it does not assert instruction-level synchronisation after startup. `--frames 600,1500,2100,3000,4500` chooses samples, and `--audio-window 14.5:20` chooses native WAV seconds around the coin sounds. Coin-to-command timing and free-running chip phase still need tracing independently.

Exit 0 requires all sampled pictures to match exactly, stereo audio correlation of at least 0.98 and RMS differences within 1 dB, a byte-identical native trace replay, and passing independent MAME chip replays. These are strict comparison criteria, not an established passing baseline. Exit 1 means measured differences (read the report); exit 2 means setup/capture failed. Silent audio, missing files and truncated captures are errors. This is an opt-in diagnostic; CI cannot run it without private ROMs and the pinned external MAME installation.

## Measured baseline, 20 November 2026

Linux x64 Release, original Ubuntu MAME 0.264, fresh state, default script:

| MAME frame | Native frame | Different RGB pixels / 196,608 |
| --- | --- | --- |
| 600 | 596 | 0 |
| 1500 | 1496 | 0 |
| 2100 | 2096 | 3,038 |
| 3000 | 2996 | 169,672 |
| 4500 | 4496 | 0 |

The 14.5–20 second coin window has stereo correlation approximately **0.746**, a best shared MAME lag of 4,207 samples at 55,930 Hz, and native RMS **0.023 dB lower**. The independent check therefore exits 1. The second picture difference includes different attract-driving state; it is not a small colour tolerance issue.

These results do not reproduce the older changelog's broad claim of matching pictures and 0.9985 audio correlation. Do not loosen thresholds to turn this into a pass. Next investigate actual input/coin read cycles, scheduling after startup, and device state with read/write traces. At this initial baseline, the board's POKEY `RANDOM` register used a cycle-count hash instead of the hardware polynomial; version 0.18 corrected that separate gap.

Separately, the audio worker preserves the synchronous model's WAV **byte for byte** over a 6,000-frame credited game with steering, throttle and speech. This establishes that moving mixing off the game thread does not introduce the independent differences above; it does not establish that the underlying mix is identical to MAME. Physical playback and MOZA R5 / Logitech G920 behaviour still need testing on the actual hardware.


## Version 0.18 follow-up traces

The POKEY RANDOM placeholder has been replaced with the hardware polynomial sequence. Repeating the same scenario leaves the five sampled pixel counts and coin correlation above unchanged. Bus traces explain why that fix alone cannot resolve this baseline: each run reads RANDOM only once, around 51.7–51.8 seconds, after the earlier attract-driving difference.

The comparison now saves `native-io.trace` / `mame-io.trace` (sound-CPU chip and latch reads/writes) and `native-input.trace` / `mame-input.trace` (main-CPU controls and ADC reads), with timestamps in 10 MHz main-CPU cycles. `report.json` includes initial sound commands, coin edges, initial RANDOM reads and YM writes, and the first sound-command value difference. The captures use ordinary Lua memory taps in original MAME; they do not modify MAME or substitute this project's device models. Raw traces can occupy hundreds of MB per run and remain local.

The first different sound-command value is index **7** (zero-based): native reads `0x0B` at main cycle **92,872,949**; MAME reads `0x16` at **93,635,513**. The sequences therefore differ during startup, before scripted coins. The first YM write occurs at cycles **87,517,045** / **87,517,131**, only 86 cycles apart, but later command scheduling diverges. First coin detection is **149,747,229** / **150,425,607**, a 678,378-cycle (67.84 ms) separation after applying the explicit four-frame input shift. Native RANDOM later returns `0x61`, MAME `0xEF`, at different chip times.

These traces establish where to investigate scheduling and phase; they do not establish that the CPU recompilation is faulty or that all residual differences have one cause. The shared-model CPU parity tests and byte-identical audio-worker comparison remain separate checks.


## Version 0.18.1: separate chip fidelity from scheduling

Native synthesis runs at the YM2151 output clock, **14,318,181 / 256 = 55,930.39453125 Hz**. Older WAV exports labelled those samples as 55,930 Hz without conversion, accumulating about 26 samples of timestamp drift by 65 seconds. WAV export now converts the exact rational source rate with a 32-tap, 1,024-phase windowed-sinc filter; output-frame counts use integer arithmetic. SDL playback starts with the matching fractional frequency ratio before its existing device-clock correction. The synthesis model and game's 64-cycle scheduler are unchanged.

`qilin_audio_trace TRACE OUTPUT.wav CHIP_STEREO_FRAMES` replays timestamped bus events through the native sound chips, speech and mixer, without executing either CPU. It needs no ROMs. The comparison harness locates this tool beside `qilin_run` (or accepts `--audio-trace`) and runs two controls:

1. Replay the native run's bus trace and require the resulting corrected WAV to match its original export **byte for byte**. A failed importer control is an error.
2. Replay original MAME's bus writes through the same native sound path and compare the result with MAME's WAV. This isolates chip/mixer fidelity from differing CPU schedules and coin-command timings.

Traces end with `# end MAIN_CYCLES`; replay rejects a missing/insufficient end marker, reversed timestamps, malformed values, invalid addresses and data after the marker. Rebuild and recapture older traces that do not have the marker. The harness still retains raw logs locally.

Measured with a fresh original-MAME 0.264 capture on 20 November 2026:

| Check | Left correlation | Right correlation | Level difference, left/right |
| --- | --- | --- | --- |
| Identical MAME writes, coin window 14.5–20 s | 0.999901 | 0.999901 | +0.0182 / +0.0182 dB |
| Identical MAME writes, long window 14.5–74 s | 0.999182 | 0.999196 | +0.0018 / +0.0025 dB |

Both replay windows align with one shared **2-sample** lag at 55,930 Hz. The native positive control is byte-identical. This supports the native chip/mixer model under the captured writes; it does not validate CPU scheduling or all possible sound programs.

The full-game coin comparison remains **0.746750** because the independently executed CPUs issue sound events at different times. The sampled picture differences remain **0, 0, 3,038, 169,672, 0**. The overall check therefore still exits **1**; isolated chip passes do not turn it green. `report.json` keeps `audio`, `chip_replay` and `images` separate. The startup-command divergence described above remains the next scheduling investigation point.

`qilin_run --quantum CYCLES` exposes the scheduler's existing positive 32-bit slice limit for controlled diagnostics. The default and the player stay at 64 cycles. Changing interleave changes CPU interactions and must be recorded when interpreting captures; this option is not a gameplay setting.

## Version 0.19.0: raster origin and independent graphics replay

Original MAME starts the screen at the beginning of vblank, line 384, at machine time zero. Its saved initial vblank end is 1.28 ms later: 32 lines × 400 main cycles. The native board previously began at visible line zero. The board now schedules line zero at cycle 12,800 and the first completed frame at cycle 166,400. The 32V interrupt tests cover the initial phase explicitly. The normal CPU scheduler retains its 64-cycle quantum.

The harness loads `tools/check/mame_video.lua` to capture original MAME's shared video pages and scroll state before scroll/list writes and at the motion-object partial-update boundaries. It captures per-band state rather than assuming that end-of-frame RAM describes the whole picture. These taps read state without altering RAM or replacing MAME's renderer. `qilin_video_trace ROM_DIR CAPTURE OUTPUT_PREFIX` then renders those bands through the native renderer without executing either native CPU. It requires the same verified private ROMs for graphics, and rejects incomplete, overlapping or inconsistent captures. The harness finds the tool beside `qilin_run`, or accepts `--video-trace PATH`.

Fresh original-MAME 0.264 capture, Linux x64 Release, 20 November 2026:

| MAME frame | Native full-game differing RGB pixels | Identical-state video replay differing RGB pixels |
| --- | --- | --- |
| 600 | 0 | 0 |
| 1500 | 0 | 0 |
| 2100 | 3,038 | 0 |
| 3000 | 169,579 | 0 |
| 4500 | 0 | 0 |

The video replay covers 35 scanline bands across those five pictures. This validates rendering for the captured states, including the mismatching full-game pictures; it does not prove every possible video state. The full-game coin correlation after the raster correction is approximately **0.785797**, still below 0.98. The overall harness still exits **1**: it requires the original full-game image/audio checks as well as independent graphics and chip replays. No threshold or picture offset has been changed to obtain a pass.

### Startup interleave probe

`mame-schedule.trace` and the report's `mame_startup_scheduling` array record the first 32 command writes/reads, machine time in main cycles and the main CPU's saved local clock. On sound-command reads, the main CPU has finished its current slice, so the difference between those clocks shows how far it has already executed ahead. On main-CPU writes, saved localtime is only the slice's starting clock and must not be interpreted as an ahead measurement.

In the measured startup, the first sound-side read of zero occurs at main cycle **87,476,505**, while the main CPU's saved local clock is already **87,489,948**: **13,443 cycles (1.3443 ms) ahead**. Its next command write, `0x09`, occurs at **87,491,151**. The native board's first zero read and next command are approximately **87,476,459** and **87,477,753**, separated by about **129.4 µs**, compared with MAME's **1.4646 ms**. This directly supports CPU interleave as a cause of the early startup response delay. It does not establish the cause of every later difference or claim that either software scheduler exactly reproduces physical hardware.

The relevant independent implementation is MAME 0.264's [CPU scheduler](https://github.com/mamedev/mame/blob/mame0264/src/emu/schedule.cpp) and [System 2 board/latch synchronisation](https://github.com/mamedev/mame/blob/mame0264/src/mame/atari/atarisy2.cpp), including its temporary 200 µs perfect-quantum request for a pending sound command. The probe uses existing saved CPU clocks and memory taps; it introduces no additional timers or memory writes. Keep full-game timing investigation separate from the already passing same-state renderer/chip checks and shared-board CPU parity.


## Version 0.20.0: ADC fidelity and controlled interleave experiments

The native ADC used to return the live selected input immediately. Original MAME's [ADC0808/0809 implementation](https://github.com/mamedev/mame/blob/mame0264/src/devices/machine/adc0808.cpp), clocked at 625 kHz by the board, instead starts with SAR `0xFF`, enters conversion-start/ready phases and samples after two ADC clocks (32 main cycles). It samples again after a further 64 clocks (1,056 cycles from the strobe), then holds that result. Channels 4–7 have unconnected callbacks and return `0xFF`, not inputs 0–3. A new strobe restarts conversion. The native model now follows those phases, including event deadlines, rather than returning a live pedal or aliasing unused channels.

`tools/check/mame_adc.lua` is an isolated probe for original MAME 0.264. Run it with autoboot delay zero, fresh configuration/NVRAM and the same ROM setup as above; require `ADC probe passed` in the log and no Lua errors. It deliberately writes an ADC selection and synthetic pedal values at startup and exits, so it must not be loaded into the normal full-game capture. Original MAME returned `0xFFFF` initially and at cycle 31, `0xFF11` at cycle 33, still `0xFF11` at 1,055, `0xFF17` at 1,057 after the pedal changed, and `0xFFFF` for disconnected channel 5. Native unit tests cover the exact 32/1,056 boundaries, result holding, restart and byte lanes.

Fresh default full-game capture with the ADC correction: differing RGB pixels **0, 0, 3,038, 52,846, 25**, coin correlation **0.769452** and overall exit **1**. One picture improved, while audio correlation fell slightly; this independent failure is retained. All five same-state graphics replays remain exact. Native chip-trace replay is byte-identical and original-MAME long-window chip replay correlations remain **0.999182 / 0.999196**. All 24,000 shared-board CPU frame hashes, the audio-worker comparison and the 6,000-frame input movie replay still match.

For an explicitly different scheduling experiment:

```sh
python tools/check/mame_compare.py --mame /path/to/mame --native build/qilin_run \
  --rom-dir /path/to/roms --out build/interleave-64 \
  --interleave-probe 64 --boot-offset 0
```

`--interleave-probe N` adds Lua timer boundaries every N main cycles to original MAME. It changes scheduler interleave without changing game memory or replacing device/CPU models. `--native-quantum N` separately changes the native scheduler's slice limit. These are diagnostic controls, not gameplay settings. Their values and `original_mame_scheduling` are recorded in every report; zero probe/64 native is the unchanged default. An experimental pass would not be an unmodified-scheduler baseline pass. All original pixel/audio criteria remain unchanged.

| Experiment | First sound read: main CPU ahead | First differing sound-command index | Coin correlation |
| --- | --- | --- | --- |
| Original MAME, native 64, four-frame offset | 13,443 cycles | 7 | 0.769452 |
| MAME 64-cycle timer probe, native 64, zero offset | 45 cycles | 37 | 0.851184 |
| MAME 16-cycle timer probe, native 16, zero offset | 18 cycles | 79 | 0.806632 |

The 64-cycle experiment sampled frames 600, 1500, 2100, 3000 and 4500; the 16-cycle experiment used 600 and 1500 and a shorter audio capture. Neither experiment passes overall. Finer interleave removes most of the early scheduler delay and postpones differing command values, but correlation does not improve monotonically. These results support an interleave contribution while leaving later divergence unresolved; they do not justify increasing the normal player's quantum or claiming physical-board cycle accuracy.

## MAME-scheduling diagnostic mode: full pass

The remaining full-game differences came from turn-taking between the two CPUs, not from the recompiled code, graphics or sound chips. To show that directly, `qilin_run --mame-scheduling 1` replaces the native 64-cycle scheduler with a port of MAME 0.264's own device scheduler (`recomp/runtime/mame/`, built into the `qilin_run` diagnostic tool only, never into the player). It copies:

- MAME's time arithmetic (`attotime`: exact multiply, divide rounding half up, `HZ_TO_ATTOSECONDS` truncation), so clocks such as the 6502's u32-truncated 1,789,772 Hz agree to the attosecond.
- The timeslice loop: each slice ends at the next timer or the quantum, each running device executes its share in device order (main CPU, sound CPU, POKEY 1, POKEY 2), a CPU that stops early pulls the slice end back, and a timer inserted at the head aborts the slice.
- Every timer APB's MAME driver creates, in the same start-up order with the same names: sound update, ADC conversion, screen vblank/scanline, latch synchronisation, motion-object force update, YM2151 mode write, the 6502's periodic IRQ, watchdog, speech ready, y-scroll reset and POKEY write/pot synchronisation.
- Queued input lines, the 200 µs perfect quantum after a sound command, and the minimum quantum of one sound-CPU cycle.
- MAME 0.264's YM2151 reset handling (reset line starts asserted; writes ignored while held) and busy flag (64 YM clocks after a data write).
- MAME 0.264's 6502 interrupt timing. The vendored reference 6502 comes from a newer MAME commit (0058568), which samples interrupts one cycle earlier and chooses BRK's vector differently. Its new `mame0264` switch, set only in this mode, restores 0.264's behaviour. The shared-board parity tests and the lifted sound code still follow the newer timing.
- MAME's speech stream timing (7,812 Hz sample spacing and per-second numbering), which decides when the TMS5220 raises ready.

The lifted T-11 code now reloads its cycle counter after every bus access, so a slice aborted during an access ends where MAME's would. In normal play nothing aborts, so this changes no result: all 16 tests, including the six ROM parity runs, still pass.

### How to run

```sh
python tools/check/mame_compare.py --mame /path/to/mame --native build/qilin_run \
  --rom-dir /path/to/roms --out build/mame-scheduling \
  --mame-scheduling --boot-offset 0
```

`--mame-scheduling` runs the native side with `--mame-scheduling 1 --mame-pixels 1` and records `native_mame_scheduling` in the report. Use `--boot-offset 0`: with identical scheduling, native frame N is MAME frame N. `--mame-pixels 1` makes the native capture show what MAME's Lua `screen:pixels()` shows at the end of frame N: the buffer drawn during frame N−1, in frame N's palette. The same applies to the graphics replay: `mame_video.lua` now pairs frame N's picture with frame N−1's bands and frame N's palette, in every mode.

### Result, 21 November 2026

Linux x64 Release, original Ubuntu MAME 0.264, fresh state, coins at 900/930, siren/start at 3100/4000, pedal 63:

| Check | Result |
| --- | --- |
| Full-game pictures, frames 600, 1500, 2100, 3000, 4500 | 0, 0, 0, 0, 0 differing RGB pixels |
| Full-game coin-window audio | correlation 0.999919 / 0.999919, level +0.0031 dB, lag 2 samples |
| Sound commands | identical; no first difference |
| Same-state graphics replay | 0, 0, 0, 0, 0 |
| MAME chip replay, coin / long window | 0.999497 / 0.999497, 0.999486 / 0.999631 |
| Overall | **exit 0, passed** |

### Checking the scheduler itself

To compare scheduling decision by decision, an instrumented MAME 0.264 (built locally with `SUBTARGET=apb`, never distributed) writes one line per timer adjustment (J), temporary timer (T), timer fired (X), slice (S), device run (R), abort (A) and quantum change (Q) when `QILIN_SCHEDLOG=file` is set, up to `QILIN_SCHEDLOG_UNTIL` seconds. `qilin_run --mame-scheduling 1 --mame-log FILE --mame-log-until S` writes the same format. The two logs were identical for 20 seconds of boot (8.88 million lines) and for 26 seconds of the coin scenario.

### What this shows

With MAME's turn-taking, the recompiled main and sound CPUs produce MAME's pictures and sound exactly over the tested scenario. The default mode's differences are therefore the native scheduler's different (finer, 64-cycle) interleave, which is a deliberate efficiency choice and not a recompilation error. The player keeps its own scheduler; this mode is a diagnostic. It throws if the game enables YM2151 timers or changes the speech clock, which APB does not do in the tested runs. It does not prove every possible game state, and it says nothing about physical-board cycle accuracy.
