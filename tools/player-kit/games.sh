# APB's details for the packaging scripts (sourced), as Toy Commander's
# games.sh. Everything game-specific in a download comes from here: the
# README templates' @NAME@ places (fill), kit/game.txt (write_game_txt),
# which is all the launcher knows about the game
# (tools/player-kit/launcher/ADOPTING.md), and the licences
# (copy_licences).
game=apb
REPO="Qilin1066/ApbArcade--QilinRecomp"
TITLE="APB"; PACKAGE="ApbArcade--QilinRecomp"; EXE="APB.exe"
BUILD_TITLE="APB - All Points Bulletin (rev 7)"   # recomp/src/identify.hpp's rev7_title
PRODUCT="MAME's apb set"
RELEASE="the rev 7 ROM set, MAME's apb: 36 files"
STUDIO="Atari Games, arcade, 1987"

# Fills a template's @NAME@ places with the game's details.
fill() {
    sed -e "s|@GAME@|$game|g" -e "s|@TITLE@|$TITLE|g" -e "s|@PACKAGE@|$PACKAGE|g" -e "s|@EXE@|$EXE|g" \
        -e "s|@BUILD_TITLE@|$BUILD_TITLE|g" -e "s|@RELEASE@|$RELEASE|g" -e "s|@STUDIO@|$STUDIO|g" "$1"
}

# Writes kit/game.txt, one name=value a line:
#   write_game_txt <file> <exe> <compiler> [<compiler_args>]
# with Windows line ends where GAME_TXT_CRLF=1. Every key is written, the
# launcher's defaults too, so the file says the whole story: the game's
# names, the data (a folder: the unzipped MAME set, which `qilin identify`
# checks file by file), how it is recompiled (both CPUs' programs, the sound
# CPU's into gen/sound), what a compile needs (the largest lifted file peaks
# under 900 MB with clang at -O3), how the game starts (apb takes the ROM
# folder as its first argument and remembers it), and where releases are
# published.
write_game_txt() {
    {
        printf 'game=%s\ntitle=%s\nexe=%s\nboot=\n' "$game" "$TITLE" "$2"
        printf 'build=%s\nproduct=%s\ndisc_version=\nrelease=%s\n' "$BUILD_TITLE" "$PRODUCT" "$RELEASE"
        printf 'package=%s\nrepo=%s\nrelease_tag_prefix=v\nrelease_asset_prefix=%s-\n' "$PACKAGE" "$REPO" "$PACKAGE"
        printf 'input=folder\ninput_noun=ROM set\n'
        printf "input_hint=Choose the folder holding the 36 files of MAME's apb set (revision 7), unzipped: 136051-1101.5t and the rest.\n"
        printf 'identify={qilin} identify {input} --json\n'
        printf 'step.1={qilin} lift {input} --out {gen}\n'
        printf 'step.2={qilin} lift {input} --cpu sound --out {gen}/sound\n'
        printf 'compile_memory_mb=1200\nbuild_space_mb=1024\ncache_mb=1024\nrun_args={input}\n'
        printf 'compiler=%s\n' "$3"
        if [ -n "${4:-}" ]; then printf 'compiler_args=%s\n' "$4"; fi
    } | if [ "${GAME_TXT_CRLF:-0}" = 1 ]; then sed 's/$/\r/'; else cat; fi > "$1"
}

# Copies the licences of what every download holds into <dir>, by component:
#   copy_licences <dir> <build dir> [crlf]
# this project's (GPL-3.0); SDL3 (zlib), fetched by the build; and everything
# vendored in third_party/, each with its own licence files: Dear ImGui and
# volk (MIT), the Vulkan headers (Apache-2.0 or MIT), ymfm and MAME's
# TMS5220 (BSD-3-Clause), which the game links, and MAME's 6502 and T-11
# (BSD-3-Clause): qilin carries the 6502's microcode (om6502.lst), the T-11
# is in no download but listed with the others. third_party/README.md says
# which file is what. The bundled compiler's are each script's own.
copy_licences() {
    lic=$1
    : "${root:?the packaging script sets root, the repository}"
    conv=cat
    if [ "${3:-}" = crlf ]; then conv="sed s/\$/\r/"; fi
    mkdir -p "$lic/$PACKAGE" "$lic/SDL3"
    $conv "$root/LICENSE" > "$lic/$PACKAGE/LICENSE.txt"
    $conv "$2/_deps/sdl3-src/LICENSE.txt" > "$lic/SDL3/LICENSE.txt"
    for d in "$root"/third_party/*/; do
        name=$(basename "$d")
        for f in "$d"LICENSE* "$d"COPYING*; do
            [ -e "$f" ] || continue
            mkdir -p "$lic/$name"
            cp -R "$f" "$lic/$name/"
        done
        [ -d "$lic/$name" ] || { echo "no licence found in third_party/$name" >&2; exit 1; }
    done
    $conv "$root/third_party/README.md" > "$lic/third-party.md"
}
