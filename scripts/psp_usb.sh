#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sdk="$root/build/psp-usb-tools/pspdev"
host="$root/dist/psp-debug/host"
mode="${1:-help}"
case "$mode" in
    usb) tool=usbhostfs_pc ;;
    shell) tool=pspsh ;;
    gdb) tool=psp-gdb ;;
    *) printf 'usage: sh scripts/psp_usb.sh [usb|shell|gdb]\n'; exit 2 ;;
esac
if [ ! -x "$sdk/bin/$tool" ]; then
    printf 'Missing PSPDEV host tool: %s\n' "$sdk/bin/$tool" >&2
    exit 1
fi
if [ ! -f "$host/rechan.prx" ] || [ ! -f "$host/rechan.elf" ]; then
    printf 'Build the isolated PRX first: sh scripts/build_psp_usb.sh\n' >&2
    exit 1
fi
cd "$host"
case "$mode" in
    usb)
        # Default listeners are loopback-only. Expose only the build output
        # directory to host0:, not the user's home or the source workspace.
        exec "$sdk/bin/usbhostfs_pc" "$host"
        ;;
    shell)
        exec "$sdk/bin/pspsh" -i 127.0.0.1 -h "$host/.pspsh.hist"
        ;;
    gdb)
        exec "$sdk/bin/psp-gdb" -q rechan.elf \
            -ex "set substitute-path /work \"$root/build/psp-usb\"" \
            -ex 'target remote 127.0.0.1:10001'
        ;;
esac
