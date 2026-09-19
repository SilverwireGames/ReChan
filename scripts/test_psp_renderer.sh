#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$(mktemp -d "${TMPDIR:-/tmp}/rechan-psp-tests.XXXXXX")
trap 'rm -rf "$output"' EXIT HUP INT TERM
for variant in fallback native native-list ram; do
    case "$variant" in
        fallback) defines="" ;;
        native) defines="-DPSP_ENABLE_NATIVE_INDEXED" ;;
        native-list) defines="-DPSP_ENABLE_NATIVE_INDEXED -DPSP_INDEXED_LIST_MEMORY" ;;
        ram) defines="-DPSP_DISABLE_STATIC_PAGE_UPLOAD" ;;
    esac
    # Intentionally split this fixed, internal defines list into arguments.
    "${CXX:-c++}" -std=c++17 -O1 -g -DNDEBUG -DRC_PLATFORM_PSP $defines \
        -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
        -I"$root/tests/psp/stubs" -I"$root/src" -I"$root/vendor/libp3d" \
        "$root/tests/psp/renderer_lifetime.cpp" -o "$output/$variant"
    "$output/$variant" > "$output/$variant.txt"
    printf '%s: ' "$variant"
    cat "$output/$variant.txt"
done
# The emitted logical geometry must be identical across submission paths.
for variant in native native-list ram; do
    test "$(sed 's/.*signature=//' "$output/fallback.txt")" = \
         "$(sed 's/.*signature=//' "$output/$variant.txt")"
done
