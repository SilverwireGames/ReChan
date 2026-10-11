#pragma once

#include "core.h"

// Bakes a 1px black outline around anti-aliased glyph coverage, so outlined
// text draws as one textured quad per glyph instead of nine.
//
// `coverage` is the w*h 8-bit glyph alpha. `out` receives RGBA8888 texels
// (R in the low byte) holding the glyph composited over its 3x3-dilated
// silhouette: alpha = a + d - a*d, and RGB is the share of that alpha that
// belongs to the glyph (white). Modulated by a vertex colour C, the texel
// reproduces "C glyph over a black ring" with ordinary alpha blending.
static inline void BakeTextOutline(const u8* coverage, s32 w, s32 h, u32* out) {
    for (s32 y = 0; y < h; ++y) {
        const s32 y0 = (y > 0) ? y - 1 : 0;
        const s32 y1 = (y + 1 < h) ? y + 1 : h - 1;
        for (s32 x = 0; x < w; ++x) {
            const s32 x0 = (x > 0) ? x - 1 : 0;
            const s32 x1 = (x + 1 < w) ? x + 1 : w - 1;

            s32 d = 0;
            for (s32 ny = y0; ny <= y1; ++ny) {
                const u8* row = coverage + ny * w;
                for (s32 nx = x0; nx <= x1; ++nx) {
                    if (row[nx] > d) {
                        d = row[nx];
                    }
                }
            }

            const s32 a = coverage[y * w + x];
            const s32 alpha = a + d - (a * d + 127) / 255;
            const s32 white = (alpha > 0) ? (a * 255 + alpha / 2) / alpha : 0;
            out[y * w + x] = ((u32)alpha << 24) | ((u32)white << 16) | ((u32)white << 8) | (u32)white;
        }
    }
}
