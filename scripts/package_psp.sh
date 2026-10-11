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

# The source tree may also contain the raw ISO, desktop launchers and
# profiling reports.  None of those are part of the native PSP runtime path.
# The .STR movies (~350 MB) stream from the Memory Stick; PSP_SKIP_MOVIES=1
# leaves them out for a smaller card, and the game then skips each movie.
# The repository keeps bin/pc as a compatibility symlink into res/pc.  Follow
# it while staging so the standalone memstick does not contain a dangling
# ../res link.
# Keep empty runtime directories: the PSP asset gate checks fe/ and tim/
# even when their contents are excluded.
movie_filter=""
if [ "${PSP_SKIP_MOVIES:-0}" = "1" ]; then
    movie_filter="--exclude=/fe/movies/***"
fi
# Only the .STR files: the movie folder may also hold desktop-only formats.
rsync -aL \
    --exclude='/discimage/***' \
    ${movie_filter:+"$movie_filter"} \
    --include='/fe/movies/*.str' --include='/fe/movies/*.STR' \
    --exclude='/fe/movies/*' \
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
# for an 8 MiB RGBA decode during boot.  The PSP frontend path is selected at
# compile time and stores the result in a 16-bit GU texture.
psp_frontend="$output_root/data/psp/textures/frontend"
mkdir -p "$psp_frontend"

generate_ptex() {
    source_path="$1"
    output_path="$2"
    target_width="$3"
    target_height="$4"
    raw_path="${output_path%.ptex}.rgba"

    mkdir -p "$(dirname "$output_path")"
    ffmpeg -hide_banner -loglevel error -y \
        -i "$source_path" \
        -vf "scale=${target_width}:${target_height}:flags=lanczos" -frames:v 1 -f rawvideo \
        -pix_fmt rgba "$raw_path"
    python3 - "$raw_path" "$output_path" "$target_width" "$target_height" <<'PY'
import struct
import sys

source_path, output_path, width_text, height_text = sys.argv[1:]
width = int(width_text)
height = int(height_text)
rgba = open(source_path, "rb").read()
expected = width * height * 4
if len(rgba) != expected:
    raise SystemExit(f"unexpected RGBA size: {len(rgba)} != {expected}")

pixels = bytearray(width * height * 2)
for src in range(0, len(rgba), 4):
    r, g, b, a = rgba[src:src + 4]
    value = (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12)
    struct.pack_into("<H", pixels, (src // 4) * 2, value)

with open(output_path, "wb") as output:
    output.write(struct.pack("<4sHHH", b"PSTX", width, height, 1))
    output.write(pixels)
PY
    rm -f "$raw_path"
}

# The five full-screen frontend layers are authored at 1920x1080.  Keep the
# source PNGs in the package for compatibility, but make the PSP path use a
# native framebuffer-sized 16-bit texture instead of decoding multi-megabyte
# RGBA images during boot.
for name in background jackie jcslogo loading_bar game_over; do
    generate_ptex \
        "$output_root/data/pc/textures/frontend/${name}.png" \
        "$psp_frontend/${name}.ptex" 480 272
done

# "PSP EDITION 1.0.0" badge: authored at 408x52, packed at 102x13 (bottom-left of
# the title screen). Keeping the 4x source only in the PC tree avoids runtime scaling.
generate_ptex \
    "$output_root/data/pc/textures/frontend/psp_edition.png" \
    "$psp_frontend/psp_edition.ptex" 102 13

# Every image used by the custom frontend/HUD is also prepared beside its
# original PC path.  LoadFromImagePath() transparently replaces .png with
# .ptex on PSP, so no desktop asset is overwritten and the PSP never falls
# back to stb_image for these assets.  Keep each source aspect ratio while
# clamping the largest side to the GE's 512-pixel texture limit.  Art that
# already fits (menu_ornament, slider_o/f at 256x256) keeps its source size:
# never upscale, since a 512x512 16-bit copy costs 512 KiB instead of 128 KiB.
psp_native_assets="
pc/textures/frontend/controller_overlay_default.png
pc/textures/frontend/controller_overlay_ps.png
pc/textures/frontend/controller_overlay_switch.png
pc/textures/frontend/controller_sheet_default.png
pc/textures/frontend/controller_sheet_ps.png
pc/textures/frontend/controller_sheet_switch.png
pc/textures/frontend/keyboard_mouse_sheet_default.png
pc/textures/frontend/menu_ornament.png
pc/textures/frontend/psp_prompts.png
pc/textures/frontend/scroll_arrow.png
pc/textures/frontend/slider_o.png
pc/textures/frontend/slider_f.png
pc/textures/frontend/clock.png
pc/textures/collectibles/bowl.png
pc/textures/collectibles/gold_dragon.png
pc/textures/collectibles/grey_dragon.png
pc/textures/collectibles/milk.png
pc/textures/collectibles/noodlebox.png
pc/textures/collectibles/red_dragon.png
pc/textures/collectibles/take.png
"

for relative_path in $psp_native_assets; do
    source_path="$output_root/data/$relative_path"
    output_path="${source_path%.png}.ptex"
    dimensions=$(ffprobe -v error -select_streams v:0 \
        -show_entries stream=width,height -of csv=p=0 "$source_path")
    source_width=${dimensions%,*}
    source_height=${dimensions#*,}
    if [ "$source_width" -le 512 ] && [ "$source_height" -le 512 ]; then
        target_width=$source_width
        target_height=$source_height
    elif [ "$source_width" -ge "$source_height" ]; then
        target_width=512
        target_height=$(( (source_height * 512 + source_width / 2) / source_width ))
    else
        target_height=512
        target_width=$(( (source_width * 512 + source_height / 2) / source_height ))
    fi
    [ "$target_width" -gt 512 ] && target_width=512
    [ "$target_height" -gt 512 ] && target_height=512
    # HUD/menu icons are displayed at a few dozen pixels.  A 256x256 native
    # copy is enough for square 512px source art and saves 75% of its PSP
    # texture memory without changing the logical UVs.
    if [ "$source_width" -eq 512 ] && [ "$source_height" -eq 512 ]; then
        target_width=256
        target_height=256
    fi
    generate_ptex "$source_path" "$output_path" "$target_width" "$target_height"
done

# Custom-menu copies packed at their on-screen size (480x272).  The menu keeps
# these resident (~11 KiB) instead of 128 KiB per 256x256 icon; the controller
# overlay (128x128, 32 KiB) is only loaded while the Controller page is open.
psp_menu="$psp_frontend/menu"
mkdir -p "$psp_menu"
generate_ptex "$output_root/data/pc/textures/frontend/slider_o.png" "$psp_menu/slider_o.ptex" 16 16
generate_ptex "$output_root/data/pc/textures/frontend/slider_f.png" "$psp_menu/slider_f.ptex" 16 16
generate_ptex "$output_root/data/pc/textures/frontend/menu_ornament.png" "$psp_menu/menu_ornament.ptex" 32 32
for name in take red_dragon gold_dragon grey_dragon; do
    generate_ptex "$output_root/data/pc/textures/collectibles/${name}.png" "$psp_menu/${name}.ptex" 32 32
done
generate_ptex "$output_root/data/pc/textures/frontend/controller_overlay_ps.png" \
    "$psp_menu/controller_overlay_ps.ptex" 128 128

echo "PSP native frontend/HUD assets: 16-bit PSTX set generated beside pc/*.png."

echo "PSP package ready: $output_root"
echo "PSX VRAM pages are converted to GU 16-bit pages on demand."
