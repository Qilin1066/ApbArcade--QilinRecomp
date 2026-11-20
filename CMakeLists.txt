cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on, so it avoids std::format, which Apple's
# libc++ only has from 13.3. The developer tools (qilin, the tests) need 13.3
# and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(ApbArcadeQilinRecomp VERSION 0.20.0 LANGUAGES C CXX)

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
add_library(qilin_persistence STATIC recomp/runtime/persistence.cpp)
target_link_libraries(qilin_persistence PUBLIC qilin_runtime PRIVATE qilin_warnings)

# The System 2 board as both CPUs see it: run by the reference run now, by the game later.
# MAME's TMS5220, adapted to run outside its device framework (built with its own warnings).
add_library(qilin_tms5220 STATIC third_party/mame-tms5220/tms5220_core.cpp)
target_include_directories(qilin_tms5220 SYSTEM PUBLIC third_party/mame-tms5220)
target_compile_features(qilin_tms5220 PUBLIC cxx_std_17)
add_library(qilin_board STATIC recomp/runtime/system2_machine.cpp recomp/runtime/system2_speech.cpp)
target_link_libraries(qilin_board PUBLIC qilin_runtime qilin_tms5220 PRIVATE qilin_warnings)
# Sound as MAME makes it: ymfm's YM2151 (vendored, unmodified), the POKEYs, the mixer.
add_library(qilin_input_movie STATIC recomp/runtime/input_movie.cpp)
target_link_libraries(qilin_input_movie PUBLIC qilin_board PRIVATE qilin_warnings)
target_compile_definitions(qilin_input_movie PRIVATE QILIN_MOVIE_VERSION="${PROJECT_VERSION}")

add_library(qilin_ymfm STATIC third_party/ymfm/ymfm_opm.cpp)
target_include_directories(qilin_ymfm SYSTEM PUBLIC third_party/ymfm)
target_compile_features(qilin_ymfm PUBLIC cxx_std_14)
find_package(Threads REQUIRED)
add_library(qilin_profile STATIC recomp/runtime/profile.cpp)
target_link_libraries(qilin_profile PUBLIC qilin_runtime qilin_persistence Threads::Threads PRIVATE qilin_warnings)
add_library(qilin_audio STATIC recomp/runtime/system2_audio.cpp recomp/runtime/audio_worker.cpp)
target_link_libraries(qilin_audio PUBLIC qilin_board qilin_profile Threads::Threads PRIVATE qilin_ymfm qilin_warnings)
# Video as MAME draws it, from the board's RAM: the CPU reference renderer.
add_library(qilin_video STATIC recomp/runtime/system2_video.cpp)
target_link_libraries(qilin_video PUBLIC qilin_board qilin_core qilin_profile PRIVATE qilin_warnings)

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
target_link_libraries(qilin_tests PRIVATE qilin_core qilin_video qilin_audio qilin_persistence qilin_input_movie qilin_warnings)
target_include_directories(qilin_tests PRIVATE recomp/runtime/app tools/common)
qilin_tool(qilin_tests)
add_test(NAME qilin_tests COMMAND qilin_tests)
if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
    find_program(QILIN_POWERSHELL NAMES pwsh powershell)
    if(QILIN_POWERSHELL)
        add_executable(qilin_launcher_stub tools/check/launcher_stub.cpp)
        target_include_directories(qilin_launcher_stub PRIVATE recomp/runtime)
        target_link_libraries(qilin_launcher_stub PRIVATE qilin_warnings shell32)
        add_test(NAME qilin_launcher_check COMMAND "${QILIN_POWERSHELL}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check/launcher_check.ps1"
            -Stub "$<TARGET_FILE:qilin_launcher_stub>"
            -Launcher "${CMAKE_CURRENT_SOURCE_DIR}/tools/player-kit/play.ps1"
            -Out "${CMAKE_CURRENT_BINARY_DIR}/launcher-check/$<CONFIG>")
        set_tests_properties(qilin_launcher_check PROPERTIES TIMEOUT 60)
    endif()
endif()


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

# Independent chip/mixer checks from MAME's timestamped bus traffic; no ROMs.
add_executable(qilin_audio_trace tools/audio_trace/audio_trace.cpp)
target_include_directories(qilin_audio_trace PRIVATE tools/common)
target_link_libraries(qilin_audio_trace PRIVATE qilin_audio qilin_warnings)
qilin_tool(qilin_audio_trace)
add_test(NAME qilin_audio_trace_check COMMAND "${CMAKE_COMMAND}"
    "-DPROGRAM=$<TARGET_FILE:qilin_audio_trace>"
    "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/audio-trace-check/$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/check/audio_trace.cmake")

add_executable(qilin_video_trace tools/video_trace/video_trace.cpp)
target_link_libraries(qilin_video_trace PRIVATE qilin_video qilin_gfx qilin_warnings)
qilin_tool(qilin_video_trace)

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
    add_library(qilin_app STATIC recomp/runtime/app/audio_output.cpp recomp/runtime/app/input.cpp recomp/runtime/app/menu.cpp recomp/runtime/app/moza.cpp)
    target_include_directories(qilin_app PUBLIC recomp/runtime/app)
    target_compile_definitions(qilin_app PRIVATE QILIN_APP_VERSION="${PROJECT_VERSION}")
    target_link_libraries(qilin_app PUBLIC qilin_board qilin_gfx qilin_imgui qilin_persistence qilin_input_movie qilin_profile SDL3::SDL3 PRIVATE qilin_warnings)
    # The controls on SDL's virtual joysticks: a wheel and pedals (no window or device needed).
    add_executable(qilin_input_tests recomp/runtime/app/input_tests.cpp)
    target_link_libraries(qilin_input_tests PRIVATE qilin_app qilin_warnings)
    qilin_tool(qilin_input_tests)
    if(WIN32)
        target_link_libraries(qilin_app PUBLIC shell32)
        # Exercise the optional SDK's real DLL loader without MOZA hardware
        # or proprietary binaries. Both MSVC and the player kit use this ABI.
        add_library(qilin_mock_moza SHARED recomp/runtime/app/mock_moza.cpp recomp/runtime/app/mock_moza.def)
        add_dependencies(qilin_input_tests qilin_mock_moza)
        add_test(NAME qilin_input_tests COMMAND qilin_input_tests "$<TARGET_FILE:qilin_mock_moza>")
    else()
        add_test(NAME qilin_input_tests COMMAND qilin_input_tests)
    endif()
endif()

# ---- The player kit: everything apb needs but the code lifted from the ROMs ----
# For the Windows download (tools/player-kit): apb's main program, the
# scheduler and every library apb links, merged into one archive, with the
# headers the lifted code includes, qilin, and the flags and libraries to
# build the rest with (kit.ninja). A player's own PC lifts its ROMs and
# compiles only that code, with the same compiler, against the kit. Needs
# SDL3 linked statically (the fetched one).
option(QILIN_PLAYER_KIT "Build the player kit (tools/player-kit)" OFF)
if(QILIN_PLAYER_KIT)
    if(NOT TARGET SDL3-static OR NOT TARGET qilin_app)
        message(FATAL_ERROR "the player kit needs SDL3 linked statically: configure with QILIN_FETCH_SDL3=ON and no installed SDL3")
    endif()
    add_library(qilin_kit_main OBJECT recomp/runtime/app/apb.cpp recomp/runtime/game/game.cpp)
    target_include_directories(qilin_kit_main PRIVATE recomp/runtime/game)
    target_link_libraries(qilin_kit_main PRIVATE qilin_app qilin_core qilin_video qilin_audio qilin_gfx qilin_board SDL3::SDL3 qilin_warnings)
    set(kit_libraries qilin_app qilin_core qilin_video qilin_audio qilin_persistence qilin_input_movie qilin_profile qilin_gfx qilin_imgui qilin_volk qilin_board qilin_tms5220 qilin_ymfm SDL3-static)
    set(kit_archive "${CMAKE_BINARY_DIR}/libapb_kit.a")
    set(mri "CREATE ${kit_archive}\n")
    foreach(library IN LISTS kit_libraries)
        string(APPEND mri "ADDLIB $<TARGET_FILE:${library}>\n")
    endforeach()
    string(APPEND mri "ADDMOD $<JOIN:$<TARGET_OBJECTS:qilin_kit_main>,\nADDMOD >\nSAVE\nEND\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/apb_kit.mri" CONTENT "${mri}")
    set(kit_depends qilin_kit_main ${kit_libraries})
    add_custom_command(OUTPUT "${kit_archive}"
        COMMAND "${CMAKE_COMMAND}" "-DAR=${CMAKE_AR}" "-DMRI=${CMAKE_BINARY_DIR}/apb_kit.mri" "-DOUT=${kit_archive}"
                -P "${CMAKE_SOURCE_DIR}/tools/player-kit/merge.cmake"
        DEPENDS ${kit_depends} $<TARGET_OBJECTS:qilin_kit_main> "${CMAKE_BINARY_DIR}/apb_kit.mri"
        COMMENT "Merging the player kit's libraries" VERBATIM)
    add_custom_target(qilin_kit ALL DEPENDS "${kit_archive}")
    # The system libraries SDL3 and the renderer need (MinGW names).
    set(kit_system_libraries -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32
        -lsetupapi -lshell32 -ldinput8 -lwinspool -lcomdlg32)
    string(JOIN " " kit_system_text ${kit_system_libraries})
    # What the lifted code is compiled and linked with: kit.ninja, included by
    # the build.ninja the player's script writes ($kit is the kit's folder).
    file(WRITE "${CMAKE_BINARY_DIR}/kit.ninja"
        "# Written by CMake for the player kit (QILIN_PLAYER_KIT).\n"
        "cxxflags = -O3 -DNDEBUG -std=c++23 -w -I$kit/include\n"
        "ldflags = -static -O3\n"
        "libs = $kit/lib/libapb_kit.a ${kit_system_text}\n")
    # Linked with stand-ins for the lifted code: the kit is complete.
    add_executable(qilin_kit_link_check tools/player-kit/link_check.cpp)
    target_include_directories(qilin_kit_link_check PRIVATE recomp/runtime recomp/runtime/game third_party/mame-tms5220)
    target_link_libraries(qilin_kit_link_check PRIVATE "${kit_archive}" ${kit_system_libraries})
    add_dependencies(qilin_kit_link_check qilin_kit)

    install(TARGETS qilin RUNTIME DESTINATION kit COMPONENT player)
    install(FILES "${kit_archive}" DESTINATION kit/lib COMPONENT player)
    install(FILES "${CMAKE_BINARY_DIR}/kit.ninja" DESTINATION kit COMPONENT player)
    file(GLOB kit_headers recomp/runtime/*.hpp recomp/runtime/game/*.hpp third_party/mame-tms5220/*.h third_party/mame-tms5220/*.hxx)
    install(FILES ${kit_headers} DESTINATION kit/include COMPONENT player)
endif()

if(QILIN_GEN_DIR AND QILIN_SOUND_GEN_DIR)
    add_library(qilin_game STATIC recomp/runtime/game/game.cpp ${QILIN_GEN_SOURCES} ${QILIN_SOUND_GEN_SOURCES})
    target_include_directories(qilin_game PUBLIC recomp/runtime/game "${QILIN_GEN_DIR}" "${QILIN_SOUND_GEN_DIR}")
    target_link_libraries(qilin_game PUBLIC qilin_board qilin_runtime PRIVATE qilin_warnings)
    if(MSVC)
        # The lifted files are large (over a megabyte of source each): more
        # sections than an object file holds without /bigobj; compiled in
        # parallel; and the labels and locals every function declares, used
        # or not, aren't worth a warning each.
        target_compile_options(qilin_game PRIVATE /bigobj /MP /wd4102 /wd4189 /wd4101)
    endif()

    add_executable(qilin_run tools/run/run.cpp)
    target_include_directories(qilin_run PRIVATE tools/common)
    target_link_libraries(qilin_run PRIVATE qilin_game qilin_core qilin_video qilin_audio qilin_input_movie qilin_gfx qilin_warnings)
    qilin_tool(qilin_run)
    # In-between frames, a prototype: renders a run with frames drawn between the game's, to a video (needs ffmpeg).
    add_executable(qilin_tween tools/tween/tween.cpp)
    target_include_directories(qilin_tween PRIVATE tools/common)
    target_link_libraries(qilin_tween PRIVATE qilin_game qilin_core qilin_video qilin_gfx qilin_warnings)
    qilin_tool(qilin_tween)

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

# Optional end-to-end checks using the player's local ROMs. No game data,
# lifted code or expected frames are stored in the source tree or CI.
set(QILIN_ROM_DIR "" CACHE PATH "Local rev 7 ROM directory for native/reference gameplay tests")
if(QILIN_ROM_DIR)
    if(NOT TARGET qilin_run)
        message(FATAL_ERROR "QILIN_ROM_DIR requires both QILIN_GEN_DIR and QILIN_SOUND_GEN_DIR; lift both CPUs first")
    endif()
    get_filename_component(QILIN_ROM_DIR_ABSOLUTE "${QILIN_ROM_DIR}" ABSOLUTE)
    if(NOT IS_DIRECTORY "${QILIN_ROM_DIR_ABSOLUTE}")
        message(FATAL_ERROR "QILIN_ROM_DIR is not a directory: ${QILIN_ROM_DIR_ABSOLUTE}")
    endif()
    add_test(NAME qilin_movie_parity COMMAND "${CMAKE_COMMAND}"
        "-DNATIVE=$<TARGET_FILE:qilin_run>" "-DROM_DIR=${QILIN_ROM_DIR_ABSOLUTE}"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/movie-parity/$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/check/movie_parity.cmake")
    set_tests_properties(qilin_movie_parity PROPERTIES LABELS "rom;parity" TIMEOUT 600)
    if(Python3_Interpreter_FOUND)
        add_test(NAME qilin_video_trace_check COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check/video_trace_check.py" "$<TARGET_FILE:qilin_video_trace>"
            "${QILIN_ROM_DIR_ABSOLUTE}" "${CMAKE_CURRENT_BINARY_DIR}/video-trace-check/$<CONFIG>")
        set_tests_properties(qilin_video_trace_check PROPERTIES LABELS "rom" TIMEOUT 120)
    endif()
    add_test(NAME qilin_audio_parity
        COMMAND "${CMAKE_COMMAND}"
            "-DNATIVE=$<TARGET_FILE:qilin_run>"
            "-DROM_DIR=${QILIN_ROM_DIR_ABSOLUTE}"
            "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/audio-parity/$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/check/audio_parity.cmake")
    set_tests_properties(qilin_audio_parity PROPERTIES LABELS "rom;parity" TIMEOUT 600)
    foreach(scenario attract credited monkey selftest)
        add_test(NAME qilin_game_parity_${scenario}
            COMMAND "${CMAKE_COMMAND}"
                "-DNATIVE=$<TARGET_FILE:qilin_run>"
                "-DREFERENCE=$<TARGET_FILE:qilin_refrun>"
                "-DROM_DIR=${QILIN_ROM_DIR_ABSOLUTE}"
                "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/game-parity/$<CONFIG>/${scenario}"
                "-DSCENARIO=${scenario}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/check/game_parity.cmake")
        set_tests_properties(qilin_game_parity_${scenario} PROPERTIES LABELS "rom;parity" TIMEOUT 600)
    endforeach()
endif()
