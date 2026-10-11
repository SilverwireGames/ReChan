#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$(mktemp -d "${TMPDIR:-/tmp}/rechan-psp-movie.XXXXXX")
trap 'rm -rf "$output"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -O1 -g -DNDEBUG -DRC_PLATFORM_PSP \
    -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
    -I"$root/src" -I"$root/vendor/libp3d" \
    "$root/tests/psp/str_decode.cpp" "$root/src/psp/psp_strdecode.cpp" -o "$output/str_decode"
cd "$root"
"$output/str_decode"
