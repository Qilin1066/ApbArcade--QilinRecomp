cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on, so it avoids std::format, which Apple's
# libc++ only has from 13.3. The developer tools (qilin, the tests) need 13.3
# and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(ApbArcadeQilinComp VERSION 0.16.0 LANGUAGES C CXX)

# ---- Language: C++23 everywhere (recompiler, runtime, generated code) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "A 64-bit toolchain is required: 32-bit hosts are out of scope")
endif()

# Minimum compilers for the C++23 language and library features we use:
# GCC 14, Clang 19, Xcode 16 (AppleClang 16), Visual Studio 2022 17.10
# (std::expected, std::print, std::ranges::to, std::string::contains).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old: C++23 <print> needs GCC 14 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    # Clang 18 lacks the C++20 concepts level libstdc++ requires for std::expected.
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Clang 19 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Xcode 16 or newer")
elseif(MSVC AND MSVC_VERSION LESS 1940)
    message(FATAL_ERROR "MSVC ${MSVC_VERSION} is too old: needs Visual Studio 2022 17.10 or newer")
endif()

option(QILIN_WERROR "Treat warnings in our own code as errors" OFF)

# ---- Warnings for our own targets ----
add_library(qilin_warnings INTERFACE)
if(MSVC)
    target_compile_options(qilin_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    # fopen/strerror etc. are standard C++; MSVC's "use the _s variants" advice is not.
    target_compile_definitions(qilin_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(qilin_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE -Werror)
    endif()
endif()

# Developer tools on macOS: macOS 13.3 or later (std::format).
function(qilin_tool target)
    if(APPLE)
        target_compile_options(${target} PRIVATE -mmacosx-version-min=13.3)
        target_link_options(${target} PRIVATE -mmacosx-version-min=13.3)
    endif()
endfunction()

# ---- Runtime headers the generated code includes (board devices, CPU state) ----
add_library(qilin_runtime INTERFACE)
target_include_directories(qilin_runtime INTERFACE recomp/runtime)

# The System 2 board as both CPUs see it: run by the reference run now, by the game later.
# MAME's TMS5220, adapted to run outside its device framework (built with its own warnings).
add_library(qilin_tms5220 STATIC third_party/mame-tms5220/tms5220_core.cpp)
target_include_directories(qilin_tms5220 SYSTEM PUBLIC third_party/mame-tms5220)
target_compile_features(qilin_tms5220 PUBLIC cxx_std_17)
add_library(qilin_board STATIC recomp/runtime/system2_machine.cpp recomp/runtime/system2_speech.cpp)
target_link_libraries(qilin_board PUBLIC qilin_runtime qilin_tms5220 PRIVATE qilin_warnings)
# Sound as MAME makes it: ymfm's YM2151 (vendored, unmodified), the POKEYs, the mixer.
add_library(qilin_ymfm STATIC third_party/ymfm/ymfm_opm.cpp)
target_include_directories(qilin_ymfm SYSTEM PUBLIC third_party/ymfm)
target_compile_features(qilin_ymfm PUBLIC cxx_std_14)
add_library(qilin_audio STATIC recomp/runtime/system2_audio.cpp)
target_link_libraries(qilin_audio PUBLIC qilin_board PRIVATE qilin_ymfm qilin_warnings)
# Video as MAME draws it, from the board's RAM: the CPU reference renderer.
add_library(qilin_video STATIC recomp/runtime/system2_video.cpp)
target_link_libraries(qilin_video PUBLIC qilin_board qilin_core PRIVATE qilin_warnings)

# ---- Recompiler core ----
# The 6502 lifter's instruction bodies are MAME's microcode: om6502.lst is
# built into qilin as bytes (rewritten only when it changes).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS third_party/mame-m6502/om6502.lst)
file(READ third_party/mame-m6502/om6502.lst QILIN_OM6502_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," QILIN_OM6502_HEX "${QILIN_OM6502_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/om6502_lst.inc.tmp" "${QILIN_OM6502_HEX}\n")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/generated/om6502_lst.inc.tmp" "${CMAKE_CURRENT_BINARY_DIR}/generated/om6502_lst.inc" COPYONLY)

add_library(qilin_core STATIC
    recomp/src/cfg.cpp
    recomp/src/coverage.cpp
    recomp/src/lifter.cpp
    recomp/src/m6502_cfg.cpp
    recomp/src/m6502_decoder.cpp
    recomp/src/m6502_lifter.cpp
    recomp/src/romset.cpp
    recomp/src/t11_decoder.cpp
    recomp/src/vrange.cpp
)
target_include_directories(qilin_core PUBLIC recomp/src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(qilin_core PUBLIC qilin_runtime PRIVATE qilin_warnings)
qilin_tool(qilin_core)

add_executable(qilin recomp/src/main.cpp)
target_link_libraries(qilin PRIVATE qilin_core qilin_warnings)
target_compile_definitions(qilin PRIVATE QILIN_VERSION="${PROJECT_VERSION}")
qilin_tool(qilin)

# ---- Tests (no game files needed) ----
enable_testing()
add_executable(qilin_tests recomp/tests/tests.cpp)
target_link_libraries(qilin_tests PRIVATE qilin_core qilin_video qilin_audio qilin_warnings)
target_include_directories(qilin_tests PRIVATE recomp/runtime/app)
qilin_tool(qilin_tests)
add_test(NAME qilin_tests COMMAND qilin_tests)

# ---- Reference: MAME's T-11 core and disassembler (BSD-3-Clause), for checks only ----
add_library(qilin_mame_t11 STATIC third_party/mame-t11/t11_reference.cpp third_party/mame-t11/t11dasm.cpp)
target_include_directories(qilin_mame_t11 PUBLIC third_party/mame-t11 tools/check/mame_shim)
if(MSVC)
    target_compile_options(qilin_mame_t11 PRIVATE /W0)
else()
    target_compile_options(qilin_mame_t11 PRIVATE -w)
endif()
qilin_tool(qilin_mame_t11)

# The decoder against MAME on all 65,536 opcode words.
add_executable(qilin_t11_decoder_check tools/check/t11_decoder_check.cpp)
target_link_libraries(qilin_t11_decoder_check PRIVATE qilin_core qilin_mame_t11 qilin_warnings)
qilin_tool(qilin_t11_decoder_check)
add_test(NAME qilin_t11_decoder_check COMMAND qilin_t11_decoder_check)

# ---- Reference: MAME's 6502 core and disassembler (BSD-3-Clause), for checks only ----
# cpu/m6502/*.hxx are generated by MAME's m6502make.py and vendored, so the
# build needs no Python; qilin_mame_m6502_generated checks they are current.
add_library(qilin_mame_m6502 STATIC third_party/mame-m6502/m6502_reference.cpp third_party/mame-m6502/m6502d.cpp)
target_include_directories(qilin_mame_m6502 PUBLIC third_party/mame-m6502 tools/check/mame_shim)
if(MSVC)
    target_compile_options(qilin_mame_m6502 PRIVATE /W0 /bigobj)
else()
    target_compile_options(qilin_mame_m6502 PRIVATE -w)
endif()
qilin_tool(qilin_mame_m6502)

# The decoder against MAME on all 256 opcodes.
add_executable(qilin_m6502_decoder_check tools/check/m6502_decoder_check.cpp)
target_link_libraries(qilin_m6502_decoder_check PRIVATE qilin_core qilin_mame_m6502 qilin_warnings)
qilin_tool(qilin_m6502_decoder_check)
add_test(NAME qilin_m6502_decoder_check COMMAND qilin_m6502_decoder_check)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME qilin_mame_m6502_generated
        COMMAND ${CMAKE_COMMAND} -DPYTHON=${Python3_EXECUTABLE}
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/third_party/mame-m6502
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mame_m6502_generated
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/check/m6502_generated.cmake)
endif()

# ---- Renderer: Vulkan 1.3, loaded at run time through volk (no SDK or link-time Vulkan needed) ----
add_library(qilin_volk STATIC third_party/volk/volk.c)
target_include_directories(qilin_volk PUBLIC third_party/volk third_party/vulkan-headers/include)
target_link_libraries(qilin_volk PUBLIC ${CMAKE_DL_LIBS})
add_library(qilin_gfx STATIC recomp/runtime/gfx/renderer.cpp recomp/runtime/gfx/png.cpp)
target_include_directories(qilin_gfx PUBLIC recomp/runtime/gfx)
target_link_libraries(qilin_gfx PUBLIC qilin_volk PRIVATE qilin_warnings)
add_executable(qilin_gfx_tests recomp/runtime/gfx/gfx_tests.cpp)
target_link_libraries(qilin_gfx_tests PRIVATE qilin_gfx qilin_warnings)
add_test(NAME qilin_gfx_tests COMMAND qilin_gfx_tests)
set_tests_properties(qilin_gfx_tests PROPERTIES SKIP_RETURN_CODE 77)

# ---- Reference run: the game on MAME's CPU cores and the board model (needs your ROMs) ----
add_executable(qilin_refrun tools/refrun/refrun.cpp)
target_include_directories(qilin_refrun PRIVATE tools/common)
target_link_libraries(qilin_refrun PRIVATE qilin_core qilin_video qilin_gfx qilin_board qilin_mame_t11 qilin_mame_m6502 qilin_warnings)
qilin_tool(qilin_refrun)

# ---- Differential test: lifted code against MAME's T-11 core (needs your ROMs) ----
#   qilin lift <rom-dir> --out gen && cmake -B build -DQILIN_GEN_DIR=$PWD/gen
#   build/qilin_difftest <rom-dir>
set(QILIN_GEN_DIR "" CACHE PATH "qilin lift output directory, for the differential test")
if(QILIN_GEN_DIR)
    file(GLOB QILIN_GEN_SOURCES CONFIGURE_DEPENDS "${QILIN_GEN_DIR}/*.cpp")
    add_executable(qilin_difftest tools/difftest/difftest.cpp ${QILIN_GEN_SOURCES})
    target_include_directories(qilin_difftest PRIVATE tools/difftest "${QILIN_GEN_DIR}")
    target_link_libraries(qilin_difftest PRIVATE qilin_core qilin_mame_t11 qilin_runtime qilin_warnings)
    qilin_tool(qilin_difftest)
endif()

# ---- Differential test: lifted sound code against MAME's 6502 core (needs your ROMs) ----
#   qilin lift <rom-dir> --cpu sound --out gen-sound && cmake -B build -DQILIN_SOUND_GEN_DIR=$PWD/gen-sound
#   build/qilin_sound_difftest <rom-dir>
set(QILIN_SOUND_GEN_DIR "" CACHE PATH "qilin lift --cpu sound output directory, for the sound differential test")
if(QILIN_SOUND_GEN_DIR)
    file(GLOB QILIN_SOUND_GEN_SOURCES CONFIGURE_DEPENDS "${QILIN_SOUND_GEN_DIR}/*.cpp")
    add_executable(qilin_sound_difftest tools/difftest6502/sound_difftest.cpp ${QILIN_SOUND_GEN_SOURCES})
    target_include_directories(qilin_sound_difftest PRIVATE tools/difftest6502 "${QILIN_SOUND_GEN_DIR}")
    target_link_libraries(qilin_sound_difftest PRIVATE qilin_core qilin_mame_m6502 qilin_runtime qilin_warnings)
    qilin_tool(qilin_sound_difftest)
endif()

# ---- The lifter against MAME's core, every instruction form (no game files; in CI) ----
add_executable(qilin_lifter_fixture tools/check/lifter_fixture.cpp)
target_link_libraries(qilin_lifter_fixture PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin_lifter_fixture)
set(QILIN_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/lifter_fixture")
set(QILIN_FIXTURE_SOURCES "${QILIN_FIXTURE_DIR}/lifted_dispatch.cpp")
foreach(k RANGE 0 7)
    list(APPEND QILIN_FIXTURE_SOURCES "${QILIN_FIXTURE_DIR}/lifted_${k}.cpp")
endforeach()
add_custom_command(
    OUTPUT ${QILIN_FIXTURE_SOURCES} "${QILIN_FIXTURE_DIR}/lifted.hpp" "${QILIN_FIXTURE_DIR}/program.bin"
    COMMAND qilin_lifter_fixture "${QILIN_FIXTURE_DIR}"
    DEPENDS qilin_lifter_fixture
    VERBATIM
)
add_executable(qilin_lifter_check tools/check/lifter_check.cpp ${QILIN_FIXTURE_SOURCES})
target_include_directories(qilin_lifter_check PRIVATE tools/difftest "${QILIN_FIXTURE_DIR}")
target_link_libraries(qilin_lifter_check PRIVATE qilin_core qilin_mame_t11 qilin_runtime qilin_warnings)
qilin_tool(qilin_lifter_check)
add_test(NAME qilin_lifter_check COMMAND qilin_lifter_check "${QILIN_FIXTURE_DIR}")

# ---- The 6502 lifter against MAME's 6502 core, every instruction form (no game files; in CI) ----
add_executable(qilin_sound_lifter_fixture tools/check/sound_lifter_fixture.cpp)
target_link_libraries(qilin_sound_lifter_fixture PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin_sound_lifter_fixture)
set(QILIN_SOUND_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/sound_lifter_fixture")
set(QILIN_SOUND_FIXTURE_SOURCES "${QILIN_SOUND_FIXTURE_DIR}/sound_dispatch.cpp")
foreach(k RANGE 0 7)
    list(APPEND QILIN_SOUND_FIXTURE_SOURCES "${QILIN_SOUND_FIXTURE_DIR}/sound_${k}.cpp")
endforeach()
add_custom_command(
    OUTPUT ${QILIN_SOUND_FIXTURE_SOURCES} "${QILIN_SOUND_FIXTURE_DIR}/sound_lifted.hpp" "${QILIN_SOUND_FIXTURE_DIR}/program.bin"
    COMMAND qilin_sound_lifter_fixture "${QILIN_SOUND_FIXTURE_DIR}"
    DEPENDS qilin_sound_lifter_fixture
    VERBATIM
)
add_executable(qilin_sound_lifter_check tools/check/sound_lifter_check.cpp ${QILIN_SOUND_FIXTURE_SOURCES})
target_include_directories(qilin_sound_lifter_check PRIVATE tools/difftest6502 "${QILIN_SOUND_FIXTURE_DIR}")
target_link_libraries(qilin_sound_lifter_check PRIVATE qilin_core qilin_mame_m6502 qilin_runtime qilin_warnings)
qilin_tool(qilin_sound_lifter_check)
add_test(NAME qilin_sound_lifter_check COMMAND qilin_sound_lifter_check "${QILIN_SOUND_FIXTURE_DIR}")

# ---- The game: both CPUs' lifted code on the board model (needs your ROMs and both lifts) ----
#   qilin lift <rom-dir> --seeds S --out gen && qilin lift <rom-dir> --cpu sound --out gen-sound
#   cmake -B build -DQILIN_GEN_DIR=$PWD/gen -DQILIN_SOUND_GEN_DIR=$PWD/gen-sound
#   build/apb <rom-dir>          the game, in a window
#   build/qilin_run <rom-dir>    headless, with scripted inputs
option(QILIN_FETCH_SDL3 "Fetch SDL3 (pinned release, linked statically) when no installed SDL3 is found" ON)

# ---- The player's side: window, input, the settings menu (SDL3, Dear ImGui) ----
# SDL3 (zlib licence): an installed one, else fetched at a pinned release and
# linked statically, else no apb (with a message). The controls layer and the
# menu are built without your ROMs too, so CI compiles them and tests the
# controls on SDL's virtual joysticks.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND AND QILIN_FETCH_SDL3)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.20 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
    set(SDL3_FOUND TRUE)
endif()
if(SDL3_FOUND)
    # Dear ImGui (MIT) for the settings menu: its core and its SDL3 and
    # Vulkan backends, the latter loading Vulkan through volk as the renderer does.
    add_library(qilin_imgui STATIC
        third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_sdl3.cpp
        third_party/imgui/backends/imgui_impl_vulkan.cpp)
    target_include_directories(qilin_imgui PUBLIC third_party/imgui third_party/imgui/backends)
    target_compile_definitions(qilin_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(qilin_imgui PUBLIC qilin_volk SDL3::SDL3)
    if(MSVC)
        target_compile_options(qilin_imgui PRIVATE /W0)
    else()
        target_compile_options(qilin_imgui PRIVATE -w)
    endif()
    # The controls (any wheel, pedals, gamepads, mice) and the settings menu.
    add_library(qilin_app STATIC recomp/runtime/app/input.cpp recomp/runtime/app/menu.cpp)
    target_include_directories(qilin_app PUBLIC recomp/runtime/app)
    target_link_libraries(qilin_app PUBLIC qilin_board qilin_gfx qilin_imgui SDL3::SDL3 PRIVATE qilin_warnings)
    # The controls on SDL's virtual joysticks: a wheel and pedals (no window or device needed).
    add_executable(qilin_input_tests recomp/runtime/app/input_tests.cpp)
    target_link_libraries(qilin_input_tests PRIVATE qilin_app qilin_warnings)
    qilin_tool(qilin_input_tests)
    add_test(NAME qilin_input_tests COMMAND qilin_input_tests)
endif()

if(QILIN_GEN_DIR AND QILIN_SOUND_GEN_DIR)
    add_library(qilin_game STATIC recomp/runtime/game/game.cpp ${QILIN_GEN_SOURCES} ${QILIN_SOUND_GEN_SOURCES})
    target_include_directories(qilin_game PUBLIC recomp/runtime/game "${QILIN_GEN_DIR}" "${QILIN_SOUND_GEN_DIR}")
    target_link_libraries(qilin_game PUBLIC qilin_board qilin_runtime PRIVATE qilin_warnings)

    add_executable(qilin_run tools/run/run.cpp)
    target_include_directories(qilin_run PRIVATE tools/common)
    target_link_libraries(qilin_run PRIVATE qilin_game qilin_core qilin_video qilin_audio qilin_gfx qilin_warnings)
    qilin_tool(qilin_run)

    # The native run handed over to MAME's cores at a frame, to tell native faults from the game's own.
    add_executable(qilin_handoff tools/handoff/handoff.cpp)
    target_include_directories(qilin_handoff PRIVATE tools/common)
    target_link_libraries(qilin_handoff PRIVATE qilin_game qilin_core qilin_mame_t11 qilin_mame_m6502 qilin_warnings)
    qilin_tool(qilin_handoff)

    if(TARGET qilin_app)
        add_executable(apb recomp/runtime/app/apb.cpp)
        target_link_libraries(apb PRIVATE qilin_app qilin_game qilin_core qilin_video qilin_audio qilin_gfx SDL3::SDL3 qilin_warnings)
    else()
        message(STATUS "apb: not built (no SDL3 found and QILIN_FETCH_SDL3 is OFF)")
    endif()
endif()
