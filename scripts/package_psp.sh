#!/bin/sh
set -eu

# Build a PSP memstick tree from a user-provided, legally extracted asset tree.
# The game keeps the PSX containers for compatibility, then decodes only the
# pages used by the current scene into native 16-bit GU textures at runtime.
# This avoids shipping the original disc image and avoids wasting the PSP's
# RAM on a second, pre-expanded copy of the complete VRAM set.

asset_root="${1:-bin}"
output_root="${2:-dist/RECHAN}"

if [ ! -d "$asset_root" ]; then
    echo "asset root not found: $asset_root" >&2
    exit 1
fi
if [ ! -f "psp/EBOOT.PBP" ]; then
    echo "psp/EBOOT.PBP not found; build the PSP target first" >&2
    exit 1
fi

mkdir -p "$output_root/data"
cp psp/EBOOT.PBP "$output_root/EBOOT.PBP"

# The source tree may also contain the raw ISO, desktop launchers, profiling
# reports and the original movie payloads.  None of those are part of the
# native PSP runtime path.
rsync -a --prune-empty-dirs \
    --exclude='/discimage/***' \
    --exclude='/fe/movies/***' \
    --exclude='/PSP_LOG.TXT' \
    --exclude='/PSP_PERF.TXT' \
    --exclude='/PSP_TRACE.TXT' \
    --exclude='/rechan' \
    "$asset_root/" "$output_root/data/"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg is required to prepare the 480x272 PSP frontend assets" >&2
    exit 1
fi

# The desktop art is 1920x1080.  Keep the source assets in the package for
# compatibility, but add a native-size PSP set so the EBOOT never asks stb
# for an 8 MiB RGBA decode during boot.  The PSP backend selects this subtree
# at compile time and stores the result in a 16-bit GU texture.
psp_frontend="$output_root/data/psp/textures/frontend"
mkdir -p "$psp_frontend"
for name in background jackie jcslogo loading_bar game_over; do
    raw_path="$psp_frontend/${name}.rgba"
    ffmpeg -hide_banner -loglevel error -y \
        -i "$output_root/data/pc/textures/frontend/${name}.png" \
        -vf 'scale=480:272:flags=lanczos' -frames:v 1 -f rawvideo \
        -pix_fmt rgba "$raw_path"
    python3 - "$raw_path" "$psp_frontend/${name}.ptex" <<'PY'
import struct
import sys

source_path, output_path = sys.argv[1:3]
rgba = open(source_path, "rb").read()
expected = 480 * 272 * 4
if len(rgba) != expected:
    raise SystemExit(f"unexpected RGBA size: {len(rgba)} != {expected}")

pixels = bytearray(480 * 272 * 2)
for src in range(0, len(rgba), 4):
    r, g, b, a = rgba[src:src + 4]
    value = (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12)
    struct.pack_into("<H", pixels, (src // 4) * 2, value)

with open(output_path, "wb") as output:
    output.write(struct.pack("<4sHHH", b"PSTX", 480, 272, 1))
    output.write(pixels)
PY
    rm -f "$raw_path"
done

echo "PSP package ready: $output_root"
echo "Frontend assets: 480x272 16-bit PSTX set generated under data/psp/."
echo "PSX VRAM pages are converted to GU 16-bit pages on demand."
