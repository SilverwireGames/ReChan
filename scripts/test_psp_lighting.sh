#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$(mktemp -d "${TMPDIR:-/tmp}/rechan-psp-lighting.XXXXXX")
trap 'rm -rf "$output"' EXIT HUP INT TERM
case "$(uname -s)" in
    Darwin) gc_sections="-Wl,-dead_strip" ;;
    *) gc_sections="-Wl,--gc-sections" ;;
esac
"${CXX:-c++}" -std=c++17 -O1 -g -DNDEBUG -DRC_PLATFORM_PSP \
    -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
    -ffunction-sections -fdata-sections "$gc_sections" \
    -I"$root/src" -I"$root/vendor/libp3d" -I"$root/vendor/cgltf" -I"$root/vendor/stb" \
    "$root/tests/psp/normal_lighting.cpp" "$root/src/gen/geometry.cpp" -o "$output/lighting"
"$output/lighting"
