# Third-party code

| Library | Version | Licence | Path | Notes |
|---|---|---|---|---|
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `vulkan-sdk-1.3.296.0` | Apache-2.0 OR MIT | `vulkan-headers/` | The Vulkan C headers (`include/vulkan/*.h`, `include/vk_video/*.h`); the C++ bindings were dropped. Same copy as GBTGhoulies-Qilincomp |
| [volk](https://github.com/zeux/volk) | `vulkan-sdk-1.3.296.0` | MIT | `volk/` | Loads the Vulkan library at run time: no Vulkan SDK needed to build, and a clear message where there's no Vulkan |
| [MAME](https://github.com/mamedev/mame) DEC T-11 core | `src/devices/cpu/t11/` at `0058568411f32c8e66166b937bf555157dbdb623` | BSD-3-Clause (Aaron Giles) | `mame-t11/` | The reference CPU for checks and differential tests, never part of what players run. `t11ops.hxx`, `t11table.hxx`, `t11dasm.cpp` and `t11dasm.h` are unmodified. `t11_reference.h` and `t11_reference.cpp` adapt MAME's `t11.h` and `t11.cpp` to run outside MAME's device framework; their headers list every difference |

Vendored as plain source (no submodules) so a clone builds offline on every
platform.

MAME's licence for these files (BSD-3-Clause, as stated in each file's
`license:` line) is in `mame-t11/LICENSE`.

To update the MAME files: copy the four unmodified files from the new
revision, diff MAME's `t11.h` and `t11.cpp` against the previous revision
and carry any change into `t11_reference.*`, update the commit above, and
rerun `qilin_t11_decoder_check`.
