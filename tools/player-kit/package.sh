#!/bin/sh
# Builds the Windows download: tools/player-kit/package.sh <version> <out.zip>
#
# Cross-compiles on Linux with llvm-mingw, then lays out:
#   APB-QilinRecomp/Play APB.bat, README.txt, LICENSES/
#   APB-QilinRecomp/kit/  qilin.exe, lib/libapb_kit.a, include/, kit.ninja,
#                       play.ps1, ninja.exe, toolchain/ (llvm-mingw, trimmed), VERSION
# Nothing in it is made from ROMs: the player's PC lifts their own ROMs and
# compiles that code with the bundled compiler (play.ps1).
#
# Environment:
#   LLVM_MINGW_CROSS    the Linux-hosted llvm-mingw release, unpacked
#   LLVM_MINGW_WINDOWS  the x86_64 Windows-hosted release of the same version, unpacked
#   NINJA_WINDOWS       a folder with ninja.exe (Windows) and Ninja's COPYING
#   BUILD_DIR           where to build (default build-player-kit)
set -eu
version=$1
out=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${BUILD_DIR:-$root/build-player-kit}
stage=$(mktemp -d)
pkg="$stage/APB-QilinRecomp"

cmake -S "$root" -B "$build" -DCMAKE_TOOLCHAIN_FILE="$root/tools/player-kit/mingw-cross.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DQILIN_PLAYER_KIT=ON -DQILIN_WERROR=ON
cmake --build "$build" --target qilin qilin_kit qilin_kit_link_check --parallel
cmake --install "$build" --component player --prefix "$pkg"

"$root/tools/player-kit/trim-toolchain.sh" "$LLVM_MINGW_WINDOWS" "$pkg/kit/toolchain"
cp "$NINJA_WINDOWS/ninja.exe" "$pkg/kit/"
cp "$root/tools/player-kit/play.ps1" "$pkg/kit/"
cp "$root/tools/player-kit/Play APB.bat" "$pkg/"
cp "$root/tools/player-kit/Check controls and performance.bat" "$pkg/"
sed 's/$/\r/' "$root/tools/player-kit/README.txt" > "$pkg/README.txt"
printf '%s\n' "$version" > "$pkg/kit/VERSION"

# Every licence of what's included, by component.
lic="$pkg/LICENSES"
mkdir -p "$lic/llvm-mingw" "$lic/ninja" "$lic/SDL3"
mv "$pkg/kit/toolchain/licenses/"* "$lic/llvm-mingw/"
rmdir "$pkg/kit/toolchain/licenses"
cp "$NINJA_WINDOWS/COPYING" "$lic/ninja/"
cp "$build/_deps/sdl3-src/LICENSE.txt" "$lic/SDL3/"
for d in "$root"/third_party/*/; do
    name=$(basename "$d")
    for f in "$d"LICENSE* "$d"COPYING*; do
        [ -e "$f" ] || continue
        mkdir -p "$lic/$name"
        cp -r "$f" "$lic/$name/"
    done
done
cp "$root/third_party/README.md" "$lic/third-party.md"

rm -f "$out"
(cd "$stage" && zip -q -r -9 "$out" APB-QilinRecomp)
rm -rf "$stage"
echo "Packaged $out ($(du -h "$out" | cut -f1))"
