# The macOS download's CMake side (tools/player-kit/package-macos.sh lays out
# the app): included from CMakeLists.txt on Apple systems, after Toy
# Commander's (ToyCommander--QilinRecomp's tools/player-kit/macos.cmake).
# (The launcher's macOS part, apple_tools.cpp and ios_install.cpp, is added
# by tools/player-kit/launcher/launcher.cmake.)

# The macOS player kit (called where CMakeLists.txt makes the kit, with its
# kit_libraries, kit_dir, kit_archive and kit_cxxflags; a macro, so what it
# sets is the caller's): everything but the lifted code in one universal
# archive (arm64 and x86_64), and build-flags.txt, with which the launcher
# compiles and links the player's lifted code using Apple's clang from the
# Command Line Tools (/usr/bin/clang++, game.txt's compiler).
macro(qilin_player_kit_macos)
    if(NOT "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES OR NOT "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        message(FATAL_ERROR "the macOS player kit is universal: configure it with -DCMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\"")
    endif()
    # Apple's libtool merges archives and objects, universal ones too (Apple's ar has no MRI scripts).
    find_program(QILIN_APPLE_LIBTOOL NAMES libtool PATHS /usr/bin NO_DEFAULT_PATH REQUIRED)
    set(kit_inputs)
    foreach(library IN LISTS kit_libraries)
        list(APPEND kit_inputs "$<TARGET_FILE:${library}>")
    endforeach()
    add_custom_command(OUTPUT "${kit_archive}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${kit_dir}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${kit_archive}"
        COMMAND "${QILIN_APPLE_LIBTOOL}" -static -no_warning_for_no_symbols -o "${kit_archive}" ${kit_inputs} $<TARGET_OBJECTS:qilin_kit_main>
        DEPENDS qilin_kit_main ${kit_libraries} $<TARGET_OBJECTS:qilin_kit_main>
        COMMENT "Merging the player kit's libraries" VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(apb_kit ALL DEPENDS "${kit_archive}")
    # What SDL3 links on macOS (its CMakeLists.txt, sdl_link_dependency), and
    # iconv (SDL_iconv). The rest is the C++ library, which clang++ adds.
    set(kit_system_libraries
        -Wl,-framework,Cocoa -Wl,-framework,IOKit -Wl,-framework,ForceFeedback -Wl,-framework,Carbon
        -Wl,-framework,CoreAudio -Wl,-framework,AudioToolbox -Wl,-framework,AVFoundation -Wl,-framework,CoreMedia
        -Wl,-framework,CoreVideo -Wl,-framework,CoreBluetooth -Wl,-framework,CoreGraphics -Wl,-framework,Foundation
        -Wl,-framework,GameController -Wl,-framework,Metal -Wl,-framework,QuartzCore
        -Wl,-weak_framework,UniformTypeIdentifiers -Wl,-weak_framework,CoreHaptics -liconv)
    string(JOIN " " kit_system_text ${kit_system_libraries})
    # Read by the launcher (tools/player-kit/launcher/build.cpp): `name = value`
    # lines, $kit the kit's folder; `<name>.<arch>` follows `<name>` for the
    # Mac's own processor (host_architecture()). The game is compiled for the
    # player's Mac only: its processor's architecture and its CPU (Apple
    # clang's -mcpu=native on Apple silicon, -march=native on Intel), and for
    # the macOS it runs (clang's default, the SDK's).
    file(WRITE "${kit_dir}/build-flags.txt"
        "# Written by CMake for the player kit (QILIN_PLAYER_KIT), macOS: universal.\n"
        "cxxflags = ${kit_cxxflags}\n"
        "native.arm64 = -arch arm64 -mcpu=native\n"
        "native.x86_64 = -arch x86_64 -march=native\n"
        "ldflags = -O3\n"
        "ldflags.arm64 = -arch arm64\n"
        "ldflags.x86_64 = -arch x86_64\n"
        "libs = $kit/lib/libapb_kit.a ${kit_system_text}\n")
endmacro()
