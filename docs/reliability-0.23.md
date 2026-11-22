# 0.23.0 reliability checks

Validated on Linux x64 with GCC 14, warnings treated as errors, on 22 November
2026. ROMs are private inputs and are never included in the repository.

- Rev 7: all 36 ROM files matched the manifest by size and SHA-1.
- The full `apb` executable and the ROM-free `qilin_player_link_check` linked.
- CTest: 15 passed, one skipped (Vulkan was unavailable).
- Four 6,000-frame native/reference checks passed: attract, credited driving,
  random inputs and self-test. Every frame hash matched. These compare CPU
  execution on shared board/render models, not an independent arcade board.
- Synchronous/worker audio parity, input movie replay and video tracing passed.
- T-11 differential check: 6,440 trials, 5,018 matching, zero mismatches.
  The remaining trials were inconclusive (did not return, waited for an
  interrupt, reached unlifted code or entered RAM).
- 6502 differential check: 4,672 trials, 3,589 matching, zero mismatches.
  Remaining trials did not return or entered unlifted code/RAM/data.
- The unit suite passed with AddressSanitizer and UndefinedBehaviorSanitizer.
  LeakSanitizer was disabled because this execution environment uses ptrace.

The new Windows launcher regression uses mock lifter/compiler processes to
check content-based rebuilds and preservation of the executable and stamp when
replacement is blocked. It runs in Windows CI; it is not a compiler or real
ROM certification. Cross-platform CI also compiles and links the whole player
without ROMs; Linux CI requires Vulkan instead of accepting a renderer skip.

Physical MOZA R5/Logitech G920 behaviour, real display/audio timing and native
Windows installation still need hardware validation using the existing
`Check controls and performance.bat` workflow. No interpolation or game timing
changes are part of this pass.

To reproduce ROM-based checks after generating both lifts:

```sh
qilin verify /path/to/roms
qilin lift /path/to/roms --out gen
qilin lift /path/to/roms --cpu sound --out gen-sound
cmake -S . -B build -DQILIN_WERROR=ON \
  -DQILIN_GEN_DIR="$PWD/gen" -DQILIN_SOUND_GEN_DIR="$PWD/gen-sound" \
  -DQILIN_ROM_DIR=/path/to/roms
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/qilin_difftest /path/to/roms --trials 8
build/qilin_sound_difftest /path/to/roms --trials 64
```
