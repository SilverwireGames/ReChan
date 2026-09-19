#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$(mktemp -d "${TMPDIR:-/tmp}/rechan-psp-perf.XXXXXX")
trap 'rm -rf "$output"' EXIT HUP INT TERM
for variant in profile soak; do
    defines=""
    if [ "$variant" = soak ]; then defines="-DPSP_PERF_SOAK"; fi
    "${CXX:-c++}" -std=c++17 -O1 -g -DNDEBUG -DRC_PLATFORM_PSP -DPSP_PERF_TELEMETRY $defines \
        -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
        -I"$root/tests/psp/stubs" -I"$root/src" -I"$root/vendor/libp3d" \
        "$root/tests/psp/perf_sampling.cpp" -o "$output/$variant"
    "$output/$variant"
done
