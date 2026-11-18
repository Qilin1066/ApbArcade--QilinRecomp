cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on, so it avoids std::format, which Apple's
# libc++ only has from 13.3. The developer tools (qilin, the tests) need 13.3
# and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(ApbArcadeQilinComp VERSION 0.15.0 LANGUAGES C CXX)

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
target_link_libraries(qilin_tests PRIVATE qilin_core qilin_warnings)
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
