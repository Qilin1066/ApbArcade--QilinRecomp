# Third-party code

| Library | Version | Licence | Path | Notes |
|---|---|---|---|---|
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `vulkan-sdk-1.3.296.0` | Apache-2.0 OR MIT | `vulkan-headers/` | The Vulkan C headers (`include/vulkan/*.h`, `include/vk_video/*.h`); the C++ bindings were dropped. Same copy as GBTGhoulies-QilinRecomp |
| [volk](https://github.com/zeux/volk) | `vulkan-sdk-1.3.296.0` | MIT | `volk/` | Loads the Vulkan library at run time: no Vulkan SDK needed to build, and a clear message where there's no Vulkan |
| [MAME](https://github.com/mamedev/mame) DEC T-11 core | `src/devices/cpu/t11/` at `0058568411f32c8e66166b937bf555157dbdb623` | BSD-3-Clause (Aaron Giles) | `mame-t11/` | The reference CPU for checks and differential tests, never part of what players run. `t11ops.hxx`, `t11table.hxx`, `t11dasm.cpp` and `t11dasm.h` are unmodified. `t11_reference.h` and `t11_reference.cpp` adapt MAME's `t11.h` and `t11.cpp` to run outside MAME's device framework; their headers list every difference |
| [MAME](https://github.com/mamedev/mame) MOS 6502 core | `src/devices/cpu/m6502/` at `0058568411f32c8e66166b937bf555157dbdb623` | BSD-3-Clause (Olivier Galibert) | `mame-m6502/` | The reference sound CPU, for checks and differential tests only. `om6502.lst`, `dm6502.lst`, `m6502make.py`, `m6502d.cpp` and `m6502d.h` are unmodified; `cpu/m6502/m6502.hxx` and `cpu/m6502/m6502d.hxx` are that generator's unmodified output. `m6502_reference.h` and `m6502_reference.cpp` adapt the NMOS 6502 parts of MAME's `m6502.h` and `m6502.cpp`; their headers list every difference |
| [ymfm](https://github.com/aaronsgiles/ymfm) (as in MAME's `3rdparty/ymfm/src/`) | MAME `0058568411f32c8e66166b937bf555157dbdb623` | BSD-3-Clause (Aaron Giles) | `ymfm/` | The YM2151 that players hear: the same code MAME runs for APB's music. `ymfm.h`, `ymfm_fm.h`, `ymfm_fm.ipp`, `ymfm_opm.h` and `ymfm_opm.cpp`, unmodified; built as its own library with its own warnings, and only `system2_audio.cpp` includes it |
| [Dear ImGui](https://github.com/ocornut/imgui) | `v1.93.0` (`c1aaac65e896a45f81f47a4d68f9959e0f86c859`) | MIT (Omar Cornut) | `imgui/` | `apb`'s settings menu. The core (`imgui*.cpp`, `imgui*.h`, `imconfig.h`, the three `imstb_*.h`) and two backends (`backends/imgui_impl_sdl3.*`, `backends/imgui_impl_vulkan.*`, with the shaders' GLSL sources in `backends/vulkan/`), all unmodified; the demo, examples and other backends are left out. Built with `IMGUI_IMPL_VULKAN_USE_VOLK` so it loads Vulkan through volk as the renderer does |
| [MAME](https://github.com/mamedev/mame) TMS5220 | `src/devices/sound/` at `0058568411f32c8e66166b937bf555157dbdb623` | BSD-3-Clause (Frank Palazzolo, Aaron Giles, Jonathan Gevaryahu, Raphael Nabet, Couriersud, Michael Zapf) | `mame-tms5220/` | The speech chip players hear, and part of the board: the 6502 waits on its READY line. `tms5110r.hxx` (the coefficient and chirp tables) is unmodified. `tms5220_core.h` and `tms5220_core.cpp` adapt MAME's `tms5220.h` and `tms5220.cpp` to run outside the device framework; the header lists every difference. The chip's logic is MAME's line for line |

Vendored as plain source (no submodules) so a clone builds offline on every
platform.

Not vendored: [SDL3](https://github.com/libsdl-org/SDL) (zlib licence), for
`apb`'s window, input, sound and the settings menu. CMake uses an installed
SDL3, or fetches release 3.4.20 and links it statically
(`-DQILIN_FETCH_SDL3=OFF` never fetches; then `apb`, the menu and the controls
test aren't built).

MAME's licence for these files (BSD-3-Clause, as stated in each file's
`license:` line) is in `mame-t11/LICENSE` and `mame-m6502/LICENSE`; ymfm's is
in `ymfm/LICENSE`; the TMS5220's in `mame-tms5220/LICENSE`.

To update the TMS5220: copy `tms5110r.hxx` from the new revision, diff
MAME's `tms5220.h` and `tms5220.cpp` against the previous revision and carry
any change into `tms5220_core.*`, update the commit above, and rerun
`qilin_tests` and a reference run.

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
