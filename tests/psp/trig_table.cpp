// The PSP quarter-wave sine table (p3dSinCosPsxAngle) against libm on the
// same radian argument, over every 16-bit PSX angle.
#include "gen/common.h"
#include "p3d/p3dmath.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

int main() {
    float maxError = 0.0f;
    for (u32 a = 0; a < 65536u; ++a) {
        float s = 0.0f;
        float c = 0.0f;
        p3dSinCosPsxAngle(static_cast<s32>(a), s, c);
        const float radians = ANGLE2RAD(a);
        maxError = std::fmax(maxError, std::fabs(s - std::sin(radians)));
        maxError = std::fmax(maxError, std::fabs(c - std::cos(radians)));
        // First quadrant: the libm value itself on the PSP; a host compiler
        // may vectorize the table loop with another sinf (an ulp apart).
        if (a < 16384u && std::fabs(s - std::sin(radians)) > 2.0e-7f * (1.0f + std::fabs(s))) {
            std::fprintf(stderr, "first quadrant differs from libm (%u)\n", a);
            return 1;
        }
    }
    // Float pi/2 and pi are rounded, so the libm values near the axes carry
    // that argument error (~1e-7); the table is the exact quadrant mirror.
    if (maxError > 1.0e-6f) {
        std::fprintf(stderr, "sine table error %g\n", static_cast<double>(maxError));
        return 1;
    }
    float s = 0.0f;
    float c = 0.0f;
    p3dSinCosPsxAngle(-16384, s, c);  // -90 degrees, as s16 joint angles wrap
    if (s != -1.0f || std::fabs(c) > 1.0e-6f) {
        std::fprintf(stderr, "negative angle wrap\n");
        return 1;
    }
    std::printf("PASS: quarter-wave sine table, 65536 angles, max error %g\n",
                static_cast<double>(maxError));
    return 0;
}
