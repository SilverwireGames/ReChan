#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$(mktemp -d "${TMPDIR:-/tmp}/rechan-psp-pacing.XXXXXX")
trap 'rm -rf "$output"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -O1 -g -DNDEBUG -DRC_PLATFORM_PSP \
    -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
    -I"$root/tests/psp/stubs" -I"$root/src" -I"$root/vendor/libp3d" \
    "$root/tests/psp/frame_pacing.cpp" -o "$output/frame-pacing"
"$output/frame-pacing"
