// BakeTextOutline: a glyph drawn from the baked texels must match the glyph
// composited over a black 3x3-dilated copy of itself.
#include "pc/textoutline.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static int s_failures = 0;

static void Expect(bool ok, const char* what, s32 x, s32 y, u32 texel) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s at %d,%d (texel %08x)\n", what, (int)x, (int)y, (unsigned)texel);
        s_failures++;
    }
}

static u32 Alpha(u32 texel) { return texel >> 24; }
static u32 White(u32 texel) { return texel & 0xffu; }

int main() {
    // A fully covered pixel and a half-covered neighbour on a 7x5 grid.
    const s32 w = 7;
    const s32 h = 5;
    std::vector<u8> coverage((size_t)(w * h), 0);
    coverage[2 * w + 2] = 255;
    coverage[2 * w + 3] = 128;
    std::vector<u32> out((size_t)(w * h), 0xdeadbeefu);
    BakeTextOutline(coverage.data(), w, h, out.data());

    for (s32 y = 0; y < h; ++y) {
        for (s32 x = 0; x < w; ++x) {
            const u32 t = out[(size_t)(y * w + x)];
            const bool r = (t & 0xffu) == ((t >> 8) & 0xffu) && (t & 0xffu) == ((t >> 16) & 0xffu);
            Expect(r, "grey texel", x, y, t);
            const bool nearFull = x >= 1 && x <= 3 && y >= 1 && y <= 3;
            const bool nearHalf = x >= 2 && x <= 4 && y >= 1 && y <= 3;
            if (x == 2 && y == 2) {
                Expect(Alpha(t) == 255 && White(t) == 255, "solid glyph texel", x, y, t);
            }
            else if (x == 3 && y == 2) {
                // 128 coverage over a full ring: opaque, half glyph colour.
                Expect(Alpha(t) == 255 && White(t) == 128, "edge texel over ring", x, y, t);
            }
            else if (nearFull) {
                Expect(Alpha(t) == 255 && White(t) == 0, "black ring around solid pixel", x, y, t);
            }
            else if (nearHalf) {
                Expect(Alpha(t) == 128 && White(t) == 0, "ring follows neighbour coverage", x, y, t);
            }
            else {
                Expect(t == 0, "empty texel stays clear", x, y, t);
            }
        }
    }

    // Clamped borders: coverage on the edge must not read outside the image.
    std::vector<u8> edge(4, 0);
    edge[0] = 200;
    std::vector<u32> edgeOut(4, 0);
    BakeTextOutline(edge.data(), 2, 2, edgeOut.data());
    // A partial pixel also sits over its own ring: 200 over 200 is 243 opaque, 210/255 glyph.
    Expect(Alpha(edgeOut[0]) == 243 && White(edgeOut[0]) == 210, "corner glyph texel", 0, 0, edgeOut[0]);
    Expect(Alpha(edgeOut[3]) == 200 && White(edgeOut[3]) == 0, "corner ring texel", 1, 1, edgeOut[3]);

    if (s_failures) {
        return 1;
    }
    std::printf("PASS: baked text outline\n");
    return 0;
}
