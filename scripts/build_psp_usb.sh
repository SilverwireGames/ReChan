#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root="$root/build/psp-usb"
output_root="$root/dist/psp-debug/host"
jobs="${PSP_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 3)}"
mkdir -p "$build_root" "$output_root"
# Keep PRX/debug objects separate from the tested release objects and EBOOT.
for directory in src vendor psp; do
    mkdir -p "$build_root/$directory"
    rsync -a --exclude='.git' --exclude='*.o' --exclude='*.d' \
        --exclude='*.elf' --exclude='*.prx' --exclude='*.PBP' --exclude='*.SFO' \
        "$root/$directory/" "$build_root/$directory/"
done
defines='-DRC_PLATFORM_PSP -DPSP -DPSP_GE -DIMGUI_DISABLE -DPSP_ENABLE_NATIVE_INDEXED -DPSP_INDEXED_LIST_MEMORY -DPSP_INDEXED_PERSISTENT_INDICES -DPSP_SORT_OPAQUE_MATERIALS -DPSP_AUTO_START_PHASE1'
if [ "${PSP_BENCH_PHASE11_ONLY:-0}" = "1" ]; then
    defines="$defines -DPSP_BENCH_PHASE11_ONLY"
fi
if [ -n "${PSPDEV:-}" ] && [ -x "$PSPDEV/bin/psp-g++" ]; then
    export PSPDEV
    PSPSDK="${PSPSDK:-$PSPDEV/psp/sdk}"
    PATH="$PSPDEV/bin:$PATH"
    export PSPSDK PATH
    make -B -j"$jobs" -C "$build_root/psp" BUILD_MODE=usb-debug BUILD_PRX=1 DEFINES="$defines"
else
    docker run --rm --platform linux/amd64 -e PSP_BUILD_JOBS="$jobs" -v "$build_root:/work" -w /work/psp \
        ghcr.io/pspdev/pspdev:latest sh -c '
            export PSPDEV=/usr/local/pspdev PSPSDK=/usr/local/pspdev/psp/sdk
            export PATH=/usr/local/pspdev/bin:$PATH
            make -B -j"${PSP_BUILD_JOBS:-3}" BUILD_MODE=usb-debug BUILD_PRX=1 DEFINES="$1"
        ' build-psp-usb "$defines"
fi
cp "$build_root/psp/rechan.prx" "$output_root/rechan.prx"
cp "$build_root/psp/rechan.elf" "$output_root/rechan.elf"
cp "$build_root/psp/EBOOT.PBP" "$output_root/EBOOT.PBP"
printf 'PSPLINK executable, EBOOT and matching symbols: %s\n' "$output_root"
