#!/usr/bin/env python3
"""Render the "PSP EDITION ..." badge of the PSP title screen.

The badge is authored at 4x (52 px tall) and packed to a quarter of its size
by scripts/package_psp.sh, so the width is rounded up to a multiple of 4.

Usage: make_psp_edition_badge.py "PSP EDITION BETA 1" [output.png]
Requires Pillow and Arial Bold (bundled with macOS).
"""
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
FONT_SIZE = 43
STROKE = 3
FILL = (255, 176, 32, 255)
OUTLINE = (20, 10, 0, 255)
HEIGHT = 52
PAD_LEFT = 11
PAD_TOP = 8
PAD_RIGHT = 10


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 1
    text = sys.argv[1]
    output = sys.argv[2] if len(sys.argv) > 2 else "res/pc/textures/frontend/psp_edition.png"

    font = ImageFont.truetype(FONT, FONT_SIZE)
    probe = ImageDraw.Draw(Image.new("RGBA", (1, 1)))
    left, top, right, _ = probe.textbbox((0, 0), text, font=font, stroke_width=STROKE)
    width = PAD_LEFT + (right - left) + PAD_RIGHT
    width = (width + 3) // 4 * 4

    image = Image.new("RGBA", (width, HEIGHT), (0, 0, 0, 0))
    ImageDraw.Draw(image).text(
        (PAD_LEFT - left, PAD_TOP - top), text, font=font,
        fill=FILL, stroke_width=STROKE, stroke_fill=OUTLINE)
    image.save(output, optimize=True)
    print(f"{output}: {width}x{HEIGHT} (packed {width // 4}x{HEIGHT // 4})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
