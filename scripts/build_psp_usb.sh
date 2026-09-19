#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root="$root/build/psp-usb"
output_root="$root/dist/psp-debug/host"
mkdir -p "$build_root" "$output_root"
# Keep PRX/debug objects separate from the tested release objects and EBOOT.
for directory in src vendor psp; do
    mkdir -p "$build_root/$directory"
    rsync -a --exclude='.git' --exclude='*.o' --exclude='*.d' \
        --exclude='*.elf' --exclude='*.prx' --exclude='*.PBP' --exclude='*.SFO' \
        "$root/$directory/" "$build_root/$directory/"
done
defines='-DRC_PLATFORM_PSP -DPSP -DPSP_GE -DIMGUI_DISABLE -DPSP_ENABLE_NATIVE_INDEXED -DPSP_INDEXED_LIST_MEMORY'
if [ -n "${PSPDEV:-}" ] && [ -x "$PSPDEV/bin/psp-g++" ]; then
    export PSPDEV
    PSPSDK="${PSPSDK:-$PSPDEV/psp/sdk}"
    PATH="$PSPDEV/bin:$PATH"
    export PSPSDK PATH
    make -B -j3 -C "$build_root/psp" BUILD_MODE=usb-debug BUILD_PRX=1 DEFINES="$defines"
else
    docker run --rm --platform linux/amd64 -v "$build_root:/work" -w /work/psp \
        ghcr.io/pspdev/pspdev:latest sh -c '
            export PSPDEV=/usr/local/pspdev PSPSDK=/usr/local/pspdev/psp/sdk
            export PATH=/usr/local/pspdev/bin:$PATH
            make -B -j3 BUILD_MODE=usb-debug BUILD_PRX=1 DEFINES="$1"
        ' build-psp-usb "$defines"
fi
cp "$build_root/psp/rechan.prx" "$output_root/rechan.prx"
cp "$build_root/psp/rechan.elf" "$output_root/rechan.elf"
printf 'PSPLINK executable and matching symbols: %s\n' "$output_root"
