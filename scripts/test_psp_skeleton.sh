#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$(mktemp -d "${TMPDIR:-/tmp}/rechan-psp-skeleton.XXXXXX")
trap 'rm -rf "$output"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -O1 -g -DNDEBUG -DRC_PLATFORM_PSP \
    -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
    -I"$root/src" -I"$root/vendor/libp3d" \
    "$root/tests/psp/skeleton_build.cpp" -o "$output/skeleton"
"$output/skeleton"
