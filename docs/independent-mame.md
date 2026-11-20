# Independent MAME checks

The usual `ctest -L parity` compares lifted CPUs against MAME CPU cores using **shared** board/video models, and compares threaded vs synchronous audio. These tests catch recompilation or threading regressions, but cannot find an error shared by those board or audio models.

`tools/check/mame_compare.py` runs a separate, original MAME 0.264 executable. Its board, graphics, audio, CPU scheduling and input framework are independent of this project's models. The script captures raw RGB32 screen pixels through MAME's Lua API, applies APB's ROT270 orientation, and compares every RGB pixel with native PNG captures. It also records both stereo WAVs, aligns a coin-sound window with one shared bounded stereo lag, and reports waveform correlation, RMS and spectral band level differences. It does not search for a different matching frame for each picture.

Supply MAME 0.264, a native build with both CPUs lifted, and your extracted rev 7 ROM directory. Python dependencies are NumPy, SciPy and Pillow:

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

Exit 0 requires all sampled pictures to match exactly, stereo audio correlation of at least 0.98 and RMS differences within 1 dB. These are strict comparison criteria, not an established passing baseline. Exit 1 means measured differences (read the report); exit 2 means setup/capture failed. Silent audio, missing files and truncated captures are errors. This is an opt-in diagnostic; CI cannot run it without private ROMs and the pinned external MAME installation.

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

These results do not reproduce the older changelog's broad claim of matching pictures and 0.9985 audio correlation. Do not loosen thresholds to turn this into a pass. Next investigate actual input/coin read cycles, scheduling after startup, and device state with read/write traces. The board's POKEY `RANDOM` register currently uses a cycle-count hash instead of the hardware polynomial: that is another fidelity gap, but has not been proven to cause these particular differences.

Separately, the audio worker preserves the synchronous model's WAV **byte for byte** over a 6,000-frame credited game with steering, throttle and speech. This establishes that moving mixing off the game thread does not introduce the independent differences above; it does not establish that the underlying mix is identical to MAME. Physical playback and MOZA R5 / Logitech G920 behaviour still need testing on the actual hardware.


## Version 0.18 follow-up traces

The POKEY RANDOM placeholder has been replaced with the hardware polynomial sequence. Repeating the same scenario leaves the five sampled pixel counts and coin correlation above unchanged. Bus traces explain why that fix alone cannot resolve this baseline: each run reads RANDOM only once, around 51.7–51.8 seconds, after the earlier attract-driving difference.

The comparison now saves `native-io.trace` / `mame-io.trace` (sound-CPU chip and latch reads/writes) and `native-input.trace` / `mame-input.trace` (main-CPU controls and ADC reads), with timestamps in 10 MHz main-CPU cycles. `report.json` includes initial sound commands, coin edges, initial RANDOM reads and YM writes, and the first sound-command value difference. The captures use ordinary Lua memory taps in original MAME; they do not modify MAME or substitute this project's device models. Raw traces can occupy hundreds of MB per run and remain local.

The first different sound-command value is index **7** (zero-based): native reads `0x0B` at main cycle **92,872,949**; MAME reads `0x16` at **93,635,513**. The sequences therefore differ during startup, before scripted coins. The first YM write occurs at cycles **87,517,045** / **87,517,131**, only 86 cycles apart, but later command scheduling diverges. First coin detection is **149,747,229** / **150,425,607**, a 678,378-cycle (67.84 ms) separation after applying the explicit four-frame input shift. Native RANDOM later returns `0x61`, MAME `0xEF`, at different chip times.

These traces establish where to investigate scheduling and phase; they do not establish that the CPU recompilation is faulty or that all residual differences have one cause. The shared-model CPU parity tests and byte-identical audio-worker comparison remain separate checks.
