# ApbArcade-QilinComp

Native recompilation tooling for Atari Games' 1987 arcade **APB**, starting with the verified rev 7 ROM set. The project follows the Ghoulies project requirements for ahead-of-time C++23, Vulkan 1.3, low-end performance, and 64-bit portability. The runtime design calls for precompiled host code for both the DEC T‑11 main program and the 6502 sound program. ROMs, generated code, and locally built binaries stay with the user's copy. [GBTGhoulies-Qilincomp](https://github.com/Tecboy1066/GBTGhoulies-Qilincomp)

## Current checkpoint

- The verifier checks the sizes and SHA‑1 digests of all 36 rev 7 files.
- The T‑11 tool translates flat and banked System 2 test programs into C++23. CTest runs both programs natively and verifies bank switches, absolute memory operands, and build-time diagnostics.
- Native System 2 code can strobe the watchdog through its mirrored low-byte port. The watchdog takes board-clock ticks from the host and holds a reset request when its timer expires.
- Native code writes the IRQ enable mask and acknowledges board interrupts. An eligible IRQ saves the T‑11 stack frame and returns the handler address to the host.
- Native word operations support register-indirect, autoincrement, indexed, and indexed-deferred operands in low RAM, palette RAM, and video RAM. A bus diagnostic reports both the instruction PC and the unsupported address.
- The compiler resolves declared PC-relative ROM jump tables to precompiled branches. Native probes exercise dispatch, out-of-range indices, and the word operations BIT, BIC, and BIS.
- Register-driven bank writes dispatch to precompiled paths. The native probe checks every hardware selector value and fetches the switch instruction's last operand under the old bank mapping.
- CPU bus accesses drive the Slapstic-controlled video memory view. The synthetic probe switches views and checks that writes and byte clears reach the selected page.
- The 256 palette words support mirrored reads and writes. The palette decoder converts the stored color and intensity fields to 8-bit RGB.
- The Vulkan 1.3 probe dynamically loads the runtime, clears an offscreen attachment, copies it back, and checks all 256 pixels. It requires a graphics queue, dynamic rendering, and synchronization2.

The read-only scan of the verified rev 7 ROM resolves four jump-table targets at PC 0xF15E and expands the register-driven bank write at PC 0xF2C4. It analyzes 144 reachable locations from reset at 0x8000 before stopping at a MOVB instruction at PC 0xF2D4. The scan tracks each combination of PC and banks separately. [Project scope and roadmap](docs/SCOPE.md) · [Changelog](CHANGELOG.md)

The native T‑11 and Vulkan checks passed on Windows x64 with a GTX 1080 Ti. Both probes built for Windows ARM64.

## Target platforms

| Platform | Target |
| --- | --- |
| Windows and Linux | x86‑64 and ARM64 with a Vulkan 1.3 driver |
| Raspberry Pi 4 | 64-bit Linux and Mesa V3DV 24.3 or newer. Reference hardware for efficiency and frame pacing |
| macOS | Intel and Apple Silicon with a Metal GPU and a Vulkan 1.3-capable MoltenVK build. The Intel runtime deployment target is macOS 10.15. The developer compiler tool uses macOS 13.3 or later for libc++ formatting |

## Build and test

Use CMake 3.25 or later with GCC 14, Clang 19, Xcode 16, or Visual Studio 2022 17.10 or later. CMake fetches pinned Khronos Vulkan headers on first configure. Set `FETCHCONTENT_SOURCE_DIR_VULKANHEADERS` to an existing Vulkan-Headers checkout for an offline configure. The probe loads Vulkan at launch and builds with just the headers. On macOS set `APB_VULKAN_LIBRARY` to the installed MoltenVK library or SDK loader if it is outside the loader's search path.

~~~sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAPB_TEST_VULKAN=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
~~~

Keep `APB_TEST_VULKAN` off on machines without a Vulkan driver. Set `APB_BUILD_VULKAN_PROBE=OFF` for a CPU-only build. On a multi-config generator, look for `apb-t11-aot` under the selected configuration directory.

Verify your unpacked ROM directory locally.

~~~powershell
cmake "-DROM_DIR=D:/APB Arcade Project" -P cmake/verify-roms.cmake
~~~

Translate a small flat, little-endian T‑11 image and link the output with a host runner that includes `qilin/t11.hpp`. The origin is both the load and entry address. The subset covers MOV, CMP, ADD, SUB, CLR, CLRB, INC, DEC, TST, NOP, direct JMP, relative branches, SOB, and HALT as a probe stop. Flat code accepts R0–R6 and immediate sources for data operations. The output uses C++ operations and direct native control flow. The execution budget bounds the probe.

~~~sh
build/apb-t11-aot --version
build/apb-t11-aot build/tests/fixtures/Release/loop.bin 0x0200 build/loop.cpp
~~~

Inspect the rev 7 reset entry against the verified local ROMs.

~~~powershell
build/apb-t11-aot --inspect-system2 cmake/apb-rev7-t11.map "D:/APB Arcade Project" 0x8000 --jump-tables cmake/apb-rev7-jumps.map
~~~

Generate a C++23 translation unit for the banked test image. The resulting `run_system2` entry takes CPU registers and low RAM, and reports the stopping address and both bank numbers.

~~~sh
build/apb-t11-aot --system2 build/tests/fixtures/Release/system2.map build/tests/fixtures/Release 0x8200 build/system2.cpp --jump-tables tests/system2-jumps.map
~~~

Each line in a System 2 map contains a physical hexadecimal byte offset, the even byte lane filename, and the odd byte lane filename, separated by `|`. Filenames are resolved under the ROM directory. Both selector windows start at bank 0. Immediate selector writes give the compiler one successor bank. A write with a runtime value expands all 64 candidate banks for the selected window. An instruction fetch from a missing ROM entry reports its PC and banks.

Each line in the optional jump-table map contains the hexadecimal address of an `ADD d(Rn),PC` instruction and a decimal entry count, separated by `|`. The compiler reads that many consecutive ROM words at the displacement in the current bank context and interprets them as offsets from the PC after the extension word. It validates each target before generating code. An out-of-range native table read reports the instruction PC, both banks, and the effective word address.

## References

- ROM names, sizes, digests, and board details come from the MAME Atari System 2 driver. [MAME driver](https://github.com/mamedev/mame/blob/master/src/mame/atari/atarisy2.cpp)
- T‑11 instruction semantics come from Digital Equipment Corporation's documentation. [DCT11 User's Guide](https://www.bitsavers.org/pdf/dec/pdp11/t11/T11_UsersMan.pdf)
- Driver capabilities depend on the installed build of Mesa or MoltenVK. [Mesa 24.3 release notes](https://docs.mesa3d.org/relnotes/24.3.0.html) · [MoltenVK 1.3.0 runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/v1.3.0/Docs/MoltenVK_Runtime_UserGuide.md)
