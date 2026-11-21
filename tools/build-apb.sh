#!/bin/sh
# Builds apb in one go on Linux or macOS: the recompiler, the lift of both
# CPUs from your ROMs, then the game. Run it again after a `git pull`: only
# what changed is rebuilt (the lift leaves unchanged files alone).
#
#   tools/build-apb.sh [rom-dir] [--run]
#
# With no rom-dir it uses the folder apb last used (apb.ini). The lifted
# code goes to gen/ and gen-sound/, which git ignores: it's made from your
# ROMs and never leaves your computer.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
roms=""
run=0
for arg in "$@"; do
    case "$arg" in
        --run) run=1 ;;
        *) roms=$arg ;;
    esac
done

if [ -z "$roms" ]; then
    # The game's data folder (qilin/apb; Qilin/apb up to v0.21.0, which the
    # game moves across the first time it runs).
    case "$(uname)" in
        Darwin) data="$HOME/Library/Application Support" ;;
        *) data="${XDG_DATA_HOME:-$HOME/.local/share}" ;;
    esac
    ini="$data/qilin/apb/apb.ini"
    [ -f "$ini" ] || ini="$data/Qilin/apb/apb.ini"
    if [ -f "$ini" ]; then
        roms=$(sed -n 's/^rom_dir = //p' "$ini" | head -n 1)
    fi
fi
if [ -z "$roms" ]; then
    echo "usage: tools/build-apb.sh <rom-dir> [--run]   (the folder with your APB ROMs)" >&2
    exit 2
fi
if [ ! -d "$roms" ]; then
    echo "no such folder: $roms" >&2
    exit 1
fi

step() { printf '\n== %s\n' "$1"; }
build="$root/build-apb"
step "Configuring"
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
step "Building the recompiler"
cmake --build "$build" --target qilin --parallel
step "Checking the ROMs"
"$build/qilin" verify "$roms"
step "Lifting the main CPU (T-11)"
"$build/qilin" lift "$roms" --out "$root/gen"
step "Lifting the sound CPU (6502)"
"$build/qilin" lift "$roms" --cpu sound --out "$root/gen-sound"
step "Configuring with the lifted code"
cmake -S "$root" -B "$build" -DQILIN_GEN_DIR="$root/gen" -DQILIN_SOUND_GEN_DIR="$root/gen-sound"
step "Building apb (the first time takes a while)"
cmake --build "$build" --target apb --parallel

printf '\nBuilt: %s\nRun it with: "%s"   (it remembers the ROM folder)\n' "$build/apb" "$build/apb"
if [ "$run" = 1 ]; then exec "$build/apb" "$roms"; fi
