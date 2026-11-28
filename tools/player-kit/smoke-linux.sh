#!/bin/sh
# Checks a built Linux download as a player would get it, without ROMs and
# without starting the launcher's window or the game (as Toy Commander's):
#   tools/player-kit/smoke-linux.sh <download.tar.gz> [oldest glibc, default 2.35]
#
# Unpacks it into an empty folder, then: the layout and kit/files.txt (both
# ways); the bundled zig runs, and with build-flags.txt as the launcher
# reads it ($kit, the native line) compiles, links and runs a small C++23
# program; the kit's library links into the game with empty dispatch tables
# (tools/player-kit/link_check.cpp, as the launcher links the lifted code),
# which starts and stops at its usage; the recompiler starts and answers the
# identify contract for a stand-in ROM folder; and the launcher, the
# recompiler and that program need nothing newer than the given glibc, and
# no C++ or SDL library from the system.
set -eu
tarball=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
floor=${2:-2.35}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
fail() { echo "smoke test: $*" >&2; exit 1; }

mkdir "$scratch/unpacked"
tar -xzf "$tarball" -C "$scratch/unpacked"
set -- "$scratch"/unpacked/*
[ $# -eq 1 ] && [ -d "$1" ] || fail "the download should hold one folder"
top=$1
kit="$top/kit"
title=$(sed -n 's/^title=//p' "$kit/game.txt")
exe=$(sed -n 's/^exe=//p' "$kit/game.txt")
[ -n "$title" ] && [ -n "$exe" ] || fail "kit/game.txt has no title or exe"
case $exe in *.exe) fail "game.txt's exe is $exe: no .exe on Linux" ;; esac
grep -qx 'compiler=toolchain/zig/zig' "$kit/game.txt" && grep -qx 'compiler_args=c++' "$kit/game.txt" || fail "game.txt doesn't name zig as the compiler"
grep -qx 'input=folder' "$kit/game.txt" || fail "game.txt's input isn't a folder"
for f in "Play $title" README.txt kit/qilin kit/build-flags.txt kit/files.txt kit/VERSION kit/icon.png kit/toolchain/zig/zig \
         kit/lib/libapb_kit.a kit/include/qilin_board.hpp kit/include/qilin_sound_board.hpp; do
    [ -f "$top/$f" ] || fail "no $f"
done
for f in "Play $title" kit/qilin kit/toolchain/zig/zig; do [ -x "$top/$f" ] || fail "$f isn't executable"; done
for f in zig/LICENSE zig/lib/libcxx/LICENSE.TXT SDL3/LICENSE.txt imgui/LICENSE.txt volk/LICENSE.md vulkan-headers/LICENSE.md \
         ymfm/LICENSE mame-tms5220/LICENSE mame-m6502/LICENSE ApbArcade--QilinRecomp/LICENSE.txt third-party.md; do
    [ -s "$top/LICENSES/$f" ] || fail "no LICENSES/$f"
done
head -c 8 "$kit/icon.png" | od -An -tx1 | tr -d ' \n' | grep -q '^89504e470d0a1a0a$' || fail "kit/icon.png isn't a PNG"
# files.txt names every file, and only those.
(cd "$top" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) > "$scratch/present.txt"
LC_ALL=C sort "$kit/files.txt" > "$scratch/listed.txt"
cmp -s "$scratch/present.txt" "$scratch/listed.txt" || { diff "$scratch/listed.txt" "$scratch/present.txt" | head -20; fail "kit/files.txt doesn't match the files"; }
echo "layout: ok ($(wc -l < "$kit/files.txt") files)"

# zig, with a cache of its own, as on a player's computer the first time.
export ZIG_GLOBAL_CACHE_DIR="$scratch/zig-cache" ZIG_LOCAL_CACHE_DIR="$scratch/zig-cache"
zig="$kit/toolchain/zig/zig"
"$zig" c++ --version | head -1

# build-flags.txt as the launcher reads it (build.cpp): `name = value`, $kit the kit.
flag() { sed -n "s/^$1[[:space:]]*=[[:space:]]*//p" "$kit/build-flags.txt" | sed "s|\\\$kit|$kit|g"; }
cxxflags=$(flag cxxflags)
native=$(flag native)
ldflags=$(flag ldflags)
libs=$(flag libs)
[ -n "$cxxflags" ] && [ -n "$libs" ] || fail "build-flags.txt has no cxxflags or libs"
[ -n "$native" ] || fail "build-flags.txt has no native line"
echo "native: $native"

cat > "$scratch/hello.cpp" <<'EOF'
#include <atomic>
#include <cstdio>
#include <expected>
#include <string>
#include <thread>
#include <vector>
static std::expected<int, std::string> half(int n) {
    if (n % 2) return std::unexpected(std::string("odd"));
    return n / 2;
}
int main() {
    std::vector<std::thread> pool;
    std::atomic<int> sum{0};
    for (int i = 0; i < 4; ++i) pool.emplace_back([&sum, i] { sum += i; });
    for (auto& t : pool) t.join();
    const auto h = half(84);
    std::printf("hello %d %s\n", h ? *h + sum.load() : -1, half(3) ? "?" : half(3).error().c_str());
    return 0;
}
EOF
cd "$scratch"
# shellcheck disable=SC2086
"$zig" c++ $cxxflags $native -c hello.cpp -o hello.o
# shellcheck disable=SC2086
"$zig" c++ $ldflags -o hello hello.o $libs
[ "$(./hello)" = "hello 48 odd" ] || fail "the compiled program said: $(./hello)"
echo "compile, link and run: ok"

# The kit's library with empty dispatch tables: the launcher's link. Started
# with an option it doesn't know, the game stops at its usage (exit 2,
# recomp/runtime/app/apb.cpp), having loaded every library it links.
# shellcheck disable=SC2086
"$zig" c++ $cxxflags $native -c "$here/link_check.cpp" -o link_check.o
# shellcheck disable=SC2086
"$zig" c++ $ldflags -o "$exe" link_check.o $libs
set +e
"./$exe" --no-such-option > run.txt 2>&1
code=$?
set -e
[ "$code" -eq 2 ] && grep -q '^usage: apb' run.txt || { cat run.txt; fail "the stand-in game ended with $code, not its usage"; }
echo "kit link and start: ok ($(du -h "$exe" | cut -f1))"

# The recompiler starts, and answers the identify contract for a stand-in
# ROM folder: one of APB's files, but not its contents (exit 2, the file
# named), and for a file that isn't a folder (exit 2).
"$kit/qilin" --version
mkdir roms
printf 'not a ROM' > roms/136051-1101.5t
set +e
"$kit/qilin" identify roms --json > identify.json 2> identify.log
code=$?
set -e
[ "$code" -eq 2 ] && grep -q '"ok": false' identify.json && grep -q '136051-1101.5t' identify.json || { cat identify.json identify.log; fail "qilin identify said $code"; }
set +e
"$kit/qilin" identify roms/136051-1101.5t --json > identify.json 2> identify.log
code=$?
set -e
[ "$code" -eq 2 ] && grep -q 'not a folder' identify.json || { cat identify.json identify.log; fail "qilin identify on a file said $code"; }
echo "recompiler: ok"

# Nothing newer than the oldest glibc, and only the system's own libraries.
for f in "$top/Play $title" "$kit/qilin" "$scratch/$exe"; do
    newest=$(objdump -T "$f" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
    [ "$(printf '%s\n%s\n' "$newest" "$floor" | sort -V | tail -1)" = "$floor" ] || fail "$(basename "$f") needs glibc $newest (newer than $floor)"
    needed=$(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')
    for n in $needed; do
        case $n in
        libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|ld-linux*.so.*) ;;
        *) fail "$(basename "$f") needs $n from the system" ;;
        esac
    done
    echo "$(basename "$f"): glibc $newest at most; needs $needed"
done
echo "smoke test: passed"
