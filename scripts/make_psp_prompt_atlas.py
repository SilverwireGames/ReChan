#!/usr/bin/env python3
"""Build res/pc/textures/frontend/psp_prompts.png, the PSP button-prompt atlas.

The face buttons and the D-pad come from the PlayStation prompt sheet the
desktop build already ships (controller_sheet_ps.png), reduced to the exact
pixel sizes the PSP text uses so the GE draws them 1:1. The D-pad also gets
one variant per direction with the pressed arm lit, and rimmed discs give
the caps of the START/SELECT/L/R badges the text renderer draws.

Layout (must match PromptIcons on PSP, src/extra/prompticons.h):
  rows:    14 px glyphs at y=0, 18 px glyphs at y=16
  columns: step 20 px; cross, circle, triangle, square, D-pad, D-pad
           left/right/up/down
  badges:  16 px disc at (180, 0) and 20 px disc at (200, 0), two pixels
           taller than the glyph rows they go with

Requires Pillow. Run from the repository root:
  python3 scripts/make_psp_prompt_atlas.py
"""
import sys

from PIL import Image, ImageDraw, ImageFilter

SHEET = "res/pc/textures/frontend/controller_sheet_ps.png"
OUTPUT = "res/pc/textures/frontend/psp_prompts.png"

ROWS = ((14, 0), (18, 16))  # (glyph size, row top)
STEP = 20
COLUMNS = 9
BADGE_LEFT = COLUMNS * STEP
DISC_RGBA = (43, 43, 43, 255)    # the sheet's button disc colour
LIT_RGBA = (245, 245, 245, 255)  # the sheet's light glyph colour
RIM_RGBA = (190, 190, 190, 255)  # badge rim, visible on dark HUD bars

# controller_sheet_ps.png: face buttons on a 168 px grid from (16, 16), 96 px
# cells, in triangle, circle, cross, square order; D-pad cell at (4, 1350).
FACE_CELL = 96
FACE_ORDER = (2, 1, 0, 3)  # sheet columns for cross, circle, triangle, square
DPAD_BOX = (4, 1350, 4 + 121, 1350 + 121)


def downscale(image, size):
    # Premultiplied resampling keeps the dark disc edges free of halos.
    return image.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")


def face_glyph(sheet, sheet_column):
    x = 16 + sheet_column * 168
    return sheet.crop((x, 16, x + FACE_CELL, 16 + FACE_CELL))


def dpad_glyphs(sheet):
    dpad = sheet.crop(DPAD_BOX)
    alpha = dpad.getchannel("A")
    w, h = dpad.size
    solid = lambda x, y: alpha.getpixel((x, y)) > 128
    # Vertical bar columns (sampled near the top) and horizontal bar rows
    # (sampled near the left) bound the centre square of the cross.
    top = next(y for y in range(h) if any(solid(x, y) for x in range(w)))
    left = next(x for x in range(w) if any(solid(x, y) for y in range(h)))
    vcols = [x for x in range(w) if solid(x, top + 6)]
    hrows = [y for y in range(h) if solid(left + 6, y)]
    vx0, vx1 = vcols[0], vcols[-1]
    hy0, hy1 = hrows[0], hrows[-1]

    # Light the whole arm up to the centre, inside a thin dark rim, so the
    # direction still reads at 14 px.
    inset = 5
    inner = alpha.point(lambda a: 255 if a > 200 else 0).filter(ImageFilter.MinFilter(2 * inset + 1))
    cx = (vx0 + vx1) // 2
    cy = (hy0 + hy1) // 2
    arms = {
        "left": (0, hy0, cx - 3, hy1),
        "right": (cx + 3, hy0, w, hy1),
        "up": (vx0, 0, vx1, cy - 3),
        "down": (vx0, cy + 3, vx1, h),
    }
    glyphs = [dpad]
    for name in ("left", "right", "up", "down"):
        lit = dpad.copy()
        mask = Image.new("L", dpad.size, 0)
        ImageDraw.Draw(mask).rectangle(arms[name], fill=255)
        mask = Image.composite(inner, Image.new("L", dpad.size, 0), mask)
        lit.paste(Image.new("RGBA", dpad.size, LIT_RGBA), (0, 0), mask)
        glyphs.append(lit)
    return glyphs


def badge_disc(size):
    # The face buttons' disc colour with a one-pixel light rim on the outer
    # pixel: the HUD's prompt bars are as dark as the disc itself. Drawn at
    # 8x the final size so the rim lands on exactly one pixel.
    scale = 8
    big = size * scale
    disc = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(disc).ellipse((0, 0, big - 1, big - 1), fill=DISC_RGBA, outline=RIM_RGBA, width=scale)
    return disc.convert("RGBa").resize((size, size), Image.BOX).convert("RGBA")


def main():
    sheet = Image.open(SHEET).convert("RGBA")
    sources = [face_glyph(sheet, c) for c in FACE_ORDER]
    sources += dpad_glyphs(sheet)
    assert len(sources) == COLUMNS

    height = ROWS[-1][1] + ROWS[-1][0]
    atlas = Image.new("RGBA", (BADGE_LEFT + len(ROWS) * STEP, height), (0, 0, 0, 0))
    for row, (size, row_top) in enumerate(ROWS):
        for column, source in enumerate(sources):
            atlas.paste(downscale(source, size), (column * STEP, row_top))
        atlas.paste(badge_disc(size + 2), (BADGE_LEFT + row * STEP, 0))
    atlas.save(OUTPUT, optimize=True)
    print(f"{OUTPUT}: {atlas.size[0]}x{atlas.size[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
