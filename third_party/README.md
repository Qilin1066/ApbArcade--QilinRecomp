# Third-party code

| Library | Version | Licence | Path | Notes |
|---|---|---|---|---|
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `vulkan-sdk-1.3.296.0` | Apache-2.0 OR MIT | `vulkan-headers/` | The Vulkan C headers (`include/vulkan/*.h`, `include/vk_video/*.h`); the C++ bindings were dropped. Same copy as GBTGhoulies-Qilincomp |
| [volk](https://github.com/zeux/volk) | `vulkan-sdk-1.3.296.0` | MIT | `volk/` | Loads the Vulkan library at run time: no Vulkan SDK needed to build, and a clear message where there's no Vulkan |
| [MAME](https://github.com/mamedev/mame) DEC T-11 core | `src/devices/cpu/t11/` at `0058568411f32c8e66166b937bf555157dbdb623` | BSD-3-Clause (Aaron Giles) | `mame-t11/` | The reference CPU for checks and differential tests, never part of what players run. `t11ops.hxx`, `t11table.hxx`, `t11dasm.cpp` and `t11dasm.h` are unmodified. `t11_reference.h` and `t11_reference.cpp` adapt MAME's `t11.h` and `t11.cpp` to run outside MAME's device framework; their headers list every difference |
| [MAME](https://github.com/mamedev/mame) MOS 6502 core | `src/devices/cpu/m6502/` at `0058568411f32c8e66166b937bf555157dbdb623` | BSD-3-Clause (Olivier Galibert) | `mame-m6502/` | The reference sound CPU, for checks and differential tests only. `om6502.lst`, `dm6502.lst`, `m6502make.py`, `m6502d.cpp` and `m6502d.h` are unmodified; `cpu/m6502/m6502.hxx` and `cpu/m6502/m6502d.hxx` are that generator's unmodified output. `m6502_reference.h` and `m6502_reference.cpp` adapt the NMOS 6502 parts of MAME's `m6502.h` and `m6502.cpp`; their headers list every difference |
| [ymfm](https://github.com/aaronsgiles/ymfm) (as in MAME's `3rdparty/ymfm/src/`) | MAME `0058568411f32c8e66166b937bf555157dbdb623` | BSD-3-Clause (Aaron Giles) | `ymfm/` | The YM2151 that players hear: the same code MAME runs for APB's music. `ymfm.h`, `ymfm_fm.h`, `ymfm_fm.ipp`, `ymfm_opm.h` and `ymfm_opm.cpp`, unmodified; built as its own library with its own warnings, and only `system2_audio.cpp` includes it |

Vendored as plain source (no submodules) so a clone builds offline on every
platform.

MAME's licence for these files (BSD-3-Clause, as stated in each file's
`license:` line) is in `mame-t11/LICENSE` and `mame-m6502/LICENSE`; ymfm's is
in `ymfm/LICENSE`.

To update ymfm: copy the five files from MAME's `3rdparty/ymfm/src/` at the
new revision, update the commit above, and rerun `qilin_tests` and a
`qilin_run --wav` recording.

To update the T-11: copy the four unmodified files from the new
revision, diff MAME's `t11.h` and `t11.cpp` against the previous revision
and carry any change into `t11_reference.*`, update the commit above, and
rerun `qilin_t11_decoder_check`.

To update the 6502: copy the five unmodified files from the new revision,
regenerate the two `.hxx` files with

```
cd third_party/mame-m6502
python3 m6502make.py s m6502 om6502.lst dm6502.lst cpu/m6502/m6502.hxx
python3 m6502make.py d m6502 om6502.lst dm6502.lst cpu/m6502/m6502d.hxx
```

carry any change to MAME's `m6502.h` and `m6502.cpp` into
`m6502_reference.*`, update the commit above, and rerun
`qilin_m6502_decoder_check`. The build itself needs no Python; where CMake
finds one, the `qilin_mame_m6502_generated` test checks the vendored `.hxx`
files are what the generator makes.
