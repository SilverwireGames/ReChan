#!/bin/sh
set -eu
# DISC_IMAGE (default bin/discimage/game.bin) and REF_DIR (default bin/) add a
# byte-for-byte comparison with the desktop extractor when both exist.
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$(mktemp -d "${TMPDIR:-/tmp}/rechan-psp-extractor.XXXXXX")
trap 'rm -rf "$output"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -O1 -g -DNDEBUG -DRC_PLATFORM_PSP \
    -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
    -I"$root/tests/psp/extractor_stubs" -I"$root/src" -I"$root/vendor/libp3d" \
    "$root/tests/psp/disc_extractor.cpp" "$root/src/psp/psp_psxdiscextractor.cpp" \
    -o "$output/disc_extractor"
cd "$root"
"$output/disc_extractor"
