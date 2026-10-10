// p3dmath.h - PC float equivalents of PSX Pure3D math functions
// Original PSX: _RMVECT16 (3×s32), MATRIX (3×5 s16 Q12 fixed-point)
// PC: Vec3 (3×f32), Mat4 (4×4 f32) - same results, float precision
//
// PSX function names preserved for 1:1 mapping with decompiled code.
#pragma once

#include "p3d/vector.h"
#include "p3d/matrix.h"
#include "p3d/lvector.h"
#include <cmath>


// PSX math function equivalents (float versions)
// Names match decompiled PSX code for easy cross-reference.

// Pi
#define PI 3.14159265358979323846f
#define TWO_PI (2.0f * PI)

// PSX binary angle: 65536 = full circle (2pi)
#define ANGLE_TO_RAD (TWO_PI / 65536.0f)
#define RAD_TO_ANGLE (65536.0f / TWO_PI)

// PSX binary angle constants (0x0000-0xFFFF = 0-360 degrees)
#define PSX_ANGLE_0    0x0000
#define PSX_ANGLE_90   0x4000
#define PSX_ANGLE_180  0x8000
#define PSX_ANGLE_270  0xC000
#define PSX_ANGLE_360  0x10000
#define PSX_ANGLE_MASK 0xFFFF

// PSX 16.16 fixed-point
#define FIX16_SCALE 65536.0f
#define FIX16_INV (1.0f / 65536.0f)

// PSX 16.16 fixed-point named constants
#define FIX16_ONE   0x10000   // 1.0
#define FIX16_HALF  0x8000    // 0.5
#define FIX16_QUARTER 0x4000  // 0.25
#define FIX16_NEG_ONE ((s32)0xFFFF0000) // -1.0
#define FIX16_MAX   0x7FFFFFFF

// Sentinel values
#define INVALID_HANDLE 0xFFFF
#define INVALID_SLOT   0xFF

// Conversion macros
#define ANGLE2RAD(a) ((f32)(a) * ANGLE_TO_RAD)
#define RAD2ANGLE(r) ((s32)((r) * RAD_TO_ANGLE))
#define FIX16_TO_FLOAT(v) ((f32)(v) * FIX16_INV)
#define FLOAT_TO_FIX16(v) ((s32)((v) * FIX16_SCALE))

// Forward declaration for helpers used by p3dFillHeadingMatrix.
inline void rmV3Normalize(LVector* out, const LVector* in);

// p3dBuildRotMatrixZ / Y / X - single-axis rotation into Mat4
// PSX: builds into 3×5 s16 Q12 MATRIX. PC: builds into 4×4 float Mat4.
inline void p3dBuildRotMatrixZ(f32 angle, Mat4& m) {
    f32 c = std::cos(angle), s = std::sin(angle);
    m = Mat4(); // identity
    m.m[0] = c;  m.m[1] = s;
    m.m[4] = -s; m.m[5] = c;
}

inline void p3dBuildRotMatrixY(f32 angle, Mat4& m) {
    f32 c = std::cos(angle), s = std::sin(angle);
    m = Mat4();
    m.m[0] = c;  m.m[2] = -s;
    m.m[8] = s;  m.m[10] = c;
}

inline void p3dBuildRotMatrixX(f32 angle, Mat4& m) {
    f32 c = std::cos(angle), s = std::sin(angle);
    m = Mat4();
    m.m[5] = c;  m.m[6] = s;
    m.m[9] = -s; m.m[10] = c;
}


// p3dBuildRotMatrixZYX - PSX: 0x80093D58
// PSX params: (ax, ay, az) = (X angle, Y angle, Z angle)
// PSX computes: R = Ry(ay) * Rx(ax) * Rz(az)
// Verified against PSX decompile: m[1][2] = -sin(ax) (pitch extraction)
inline void p3dBuildRotMatrixZYX(f32 ax, f32 ay, f32 az, Mat4& m) {
    f32 cx = std::cos(ax), sx = std::sin(ax);
    f32 cy = std::cos(ay), sy = std::sin(ay);
    f32 cz = std::cos(az), sz = std::sin(az);

    m = Mat4();
    m.m[0]  = cy * cz + sy * sx * sz;
    m.m[1]  = cx * sz;
    m.m[2]  = cy * sx * sz - sy * cz;

    m.m[4]  = sy * sx * cz - cy * sz;
    m.m[5]  = cx * cz;
    m.m[6]  = sy * sz + cy * sx * cz;

    m.m[8]  = sy * cx;
    m.m[9]  = -sx;
    m.m[10] = cy * cx;
}

// Overload taking PSX angle units (s32) - converts internally
inline void p3dBuildRotMatrixZYX(s32 ax, s32 ay, s32 az, Mat4& m) {
    p3dBuildRotMatrixZYX(
        ANGLE2RAD(ax & 0xFFFF),
        ANGLE2RAD(ay & 0xFFFF),
        ANGLE2RAD(az & 0xFFFF),
        m);
}

// Keep packed PSX angle fields unambiguous with GCC: converting u16/s16 to
// either the float or integer overload is otherwise equally ranked.
inline void p3dBuildRotMatrixZYX(u16 ax, u16 ay, u16 az, Mat4& m) {
    p3dBuildRotMatrixZYX(static_cast<s32>(ax), static_cast<s32>(ay), static_cast<s32>(az), m);
}

inline void p3dBuildRotMatrixZYX(s16 ax, s16 ay, s16 az, Mat4& m) {
    p3dBuildRotMatrixZYX(static_cast<s32>(ax), static_cast<s32>(ay), static_cast<s32>(az), m);
}

// p3dBuildRotMatrixXYZ - PSX: 0x800730F8
// PSX params: (ax, ay, az) = (X angle, Y angle, Z angle)
inline void p3dBuildRotMatrixXYZ(f32 ax, f32 ay, f32 az, Mat4& m) {
    f32 cx = std::cos(ax), sx = std::sin(ax);
    f32 cy = std::cos(ay), sy = std::sin(ay);
    f32 cz = std::cos(az), sz = std::sin(az);

    m = Mat4();
    m.m[0]  = cy * cz;
    m.m[1]  = cy * sz;
    m.m[2]  = -sy;

    m.m[4]  = sx * sy * cz - cx * sz;
    m.m[5]  = sx * sy * sz + cx * cz;
    m.m[6]  = sx * cy;

    m.m[8]  = cx * sy * cz + sx * sz;
    m.m[9]  = cx * sy * sz - sx * cz;
    m.m[10] = cx * cy;
}

// Overload taking PSX angle units (s32)
inline void p3dBuildRotMatrixXYZ(s32 ax, s32 ay, s32 az, Mat4& m) {
    p3dBuildRotMatrixXYZ(
        ANGLE2RAD(ax & 0xFFFF),
        ANGLE2RAD(ay & 0xFFFF),
        ANGLE2RAD(az & 0xFFFF),
        m);
}

inline void p3dBuildRotMatrixXYZ(u16 ax, u16 ay, u16 az, Mat4& m) {
    p3dBuildRotMatrixXYZ(static_cast<s32>(ax), static_cast<s32>(ay), static_cast<s32>(az), m);
}

inline void p3dBuildRotMatrixXYZ(s16 ax, s16 ay, s16 az, Mat4& m) {
    p3dBuildRotMatrixXYZ(static_cast<s32>(ax), static_cast<s32>(ay), static_cast<s32>(az), m);
}

// p3dBuildRotMatrixYZX - PSX: 0x800737A4
// PSX params: (ax, ay, az) = (X angle, Y angle, Z angle)
// PSX computes: R = Rx(ax) * Rz(az) * Ry(ay)
// Verified against PSX decompile scratchpad implementation
inline void p3dBuildRotMatrixYZXFromSinCos(f32 cx, f32 sx, f32 cy, f32 sy, f32 cz, f32 sz,
                                           Mat4& m) {
    m = Mat4();
    m.m[0]  = cz * cy;
    m.m[1]  = cx * sz * cy + sx * sy;
    m.m[2]  = sx * sz * cy - cx * sy;

    m.m[4]  = -sz;
    m.m[5]  = cx * cz;
    m.m[6]  = sx * cz;

    m.m[8]  = cz * sy;
    m.m[9]  = cx * sz * sy - sx * cy;
    m.m[10] = sx * sz * sy + cx * cy;
}

inline void p3dBuildRotMatrixYZX(f32 ax, f32 ay, f32 az, Mat4& m) {
    p3dBuildRotMatrixYZXFromSinCos(std::cos(ax), std::sin(ax), std::cos(ay), std::sin(ay),
                                   std::cos(az), std::sin(az), m);
}

#if defined(RC_PLATFORM_PSP)
// Exact sinf/cosf of a PSX angle, memoized. Skeletons rebuild every joint's
// rotation several times per frame (logic tick, attack capture, render pose)
// from a small set of 16-bit angles; on the Allegrex each libm sinf/cosf costs
// hundreds of cycles. A hit returns the very values the libm call produced.
struct P3dSinCosEntry {
    u32 key;  // angle + 1, so the zeroed table starts empty
    f32 s;
    f32 c;
};
inline P3dSinCosEntry g_p3dSinCosCache[1024];

inline void p3dSinCosPsxAngle(s32 angle, f32& s, f32& c) {
    const u32 a = static_cast<u32>(angle) & 0xFFFFu;
    P3dSinCosEntry& entry = g_p3dSinCosCache[(a ^ (a >> 10)) & 1023u];
    if (entry.key != a + 1u) {
        const f32 radians = ANGLE2RAD(a);
        entry.s = std::sin(radians);
        entry.c = std::cos(radians);
        entry.key = a + 1u;
    }
    s = entry.s;
    c = entry.c;
}
#endif

// Overload taking PSX angle units (s32) - converts internally
inline void p3dBuildRotMatrixYZX(s32 ax, s32 ay, s32 az, Mat4& m) {
#if defined(RC_PLATFORM_PSP)
    f32 sx, cx, sy, cy, sz, cz;
    p3dSinCosPsxAngle(ax, sx, cx);
    p3dSinCosPsxAngle(ay, sy, cy);
    p3dSinCosPsxAngle(az, sz, cz);
    p3dBuildRotMatrixYZXFromSinCos(cx, sx, cy, sy, cz, sz, m);
#else
    p3dBuildRotMatrixYZX(
        ANGLE2RAD(ax & 0xFFFF),
        ANGLE2RAD(ay & 0xFFFF),
        ANGLE2RAD(az & 0xFFFF),
        m);
#endif
}

inline void p3dBuildRotMatrixYZX(u16 ax, u16 ay, u16 az, Mat4& m) {
    p3dBuildRotMatrixYZX(static_cast<s32>(ax), static_cast<s32>(ay), static_cast<s32>(az), m);
}

inline void p3dBuildRotMatrixYZX(s16 ax, s16 ay, s16 az, Mat4& m) {
    p3dBuildRotMatrixYZX(static_cast<s32>(ax), static_cast<s32>(ay), static_cast<s32>(az), m);
}

// p3dVecTimesMatrix - transform vector by matrix (rotation + translation)
// PSX: 0x80094568 - result = vec * rotPart + translation
inline Vec3 p3dVecTimesMatrix(const Vec3& v, const Mat4& m) {
    return {
        v.x * m.m[0] + v.y * m.m[4] + v.z * m.m[8]  + m.m[12],
        v.x * m.m[1] + v.y * m.m[5] + v.z * m.m[9]  + m.m[13],
        v.x * m.m[2] + v.y * m.m[6] + v.z * m.m[10] + m.m[14]
    };
}


// p3dVecTimesRotMatrix - rotation only (no translation)
// PSX: 0x800945EC
inline Vec3 p3dVecTimesRotMatrix(const Vec3& v, const Mat4& m) {
    return {
        v.x * m.m[0] + v.y * m.m[4] + v.z * m.m[8],
        v.x * m.m[1] + v.y * m.m[5] + v.z * m.m[9],
        v.x * m.m[2] + v.y * m.m[6] + v.z * m.m[10]
    };
}


// p3dBuildTransMatrix - build identity + translation
inline void p3dBuildTransMatrix(f32 x, f32 y, f32 z, Mat4& m) {
    m = Mat4();
    m.SetTranslation(x, y, z);
}


// p3dFillTransMatrix - fill translation into existing matrix (PSX: 0x800946D0)
// PSX: writes ONLY the translation part, leaves rotation untouched.
inline void p3dFillTransMatrix(const LVector& pos, Mat4& m) {
    m.SetTranslation((f32)pos.x, (f32)pos.y, (f32)pos.z);
}


// p3dFillHeadingMatrix - build orientation matrix from heading + up vectors
// PSX: 0x800947F0 - builds right/up/forward basis
inline void p3dFillHeadingMatrix(const Vec3& heading, const Vec3& up, Mat4& m) {
    const LVector headingVec = {
        FLOAT_TO_FIX16(heading.x),
        FLOAT_TO_FIX16(heading.y),
        FLOAT_TO_FIX16(heading.z),
    };
    const LVector upVec = {
        FLOAT_TO_FIX16(up.x),
        FLOAT_TO_FIX16(up.y),
        FLOAT_TO_FIX16(up.z),
    };

    // PSX parity (0x80073C8C): rmV3Normalize falls back to (1,0,0) on zero magnitude.
    LVector fwdVec = {};
    rmV3Normalize(&fwdVec, &headingVec);

    const LVector rightCross = upVec.Cross(headingVec);
    LVector rightVec = {};
    rmV3Normalize(&rightVec, &rightCross);

    const LVector realUpCross = headingVec.Cross(rightVec);
    LVector realUpVec = {};
    rmV3Normalize(&realUpVec, &realUpCross);

    const Vec3 fwd(
        FIX16_TO_FLOAT(fwdVec.x),
        FIX16_TO_FLOAT(fwdVec.y),
        FIX16_TO_FLOAT(fwdVec.z));
    const Vec3 right(
        FIX16_TO_FLOAT(rightVec.x),
        FIX16_TO_FLOAT(rightVec.y),
        FIX16_TO_FLOAT(rightVec.z));
    const Vec3 realUp(
        FIX16_TO_FLOAT(realUpVec.x),
        FIX16_TO_FLOAT(realUpVec.y),
        FIX16_TO_FLOAT(realUpVec.z));

    m = Mat4();
    m.m[0] = right.x;  m.m[1] = right.y;  m.m[2] = right.z;
    m.m[4] = realUp.x;  m.m[5] = realUp.y;  m.m[6] = realUp.z;
    m.m[8] = fwd.x;     m.m[9] = fwd.y;     m.m[10] = fwd.z;
}

// fixmul16 - 16.16 fixed-point multiply: (a * b) >> 16
// PSX: implemented inline via MIPS mult + shift
inline s32 fixmul16(s32 a, s32 b) {
    return (s32)(((s64)a * (s64)b) >> 16);
}

// rmMag2 - 2D magnitude (PSX: 0x80113984)
inline f32 rmMag2(f32 x, f32 y) {
    return std::sqrt(x * x + y * y);
}

// rmMag3 - 3D magnitude (PSX: 0x80113B90)
inline f32 rmMag3(f32 x, f32 y, f32 z) {
    return std::sqrt(x * x + y * y + z * z);
}

// rmMag3 - integer 3D magnitude with prescale to avoid overflow (PSX: rmMag3__Flll, 0x8009D6EC)
// Source: C:\chan\devsys\psx\radlib\SOURCE\MATH\FUNC\MAG3.CPP:17
inline s32 rmMag3(s32 x, s32 y, s32 z)
{
    if (x < 0) x = -x;
    if (y < 0) y = -y;
    if (z < 0) z = -z;

    u32 shift = 0;
    while (x + y + z > 46339)
    {
        x >>= 2; y >>= 2; z >>= 2;
        shift += 2;
    }

    u32 v = (u32)(x * x + y * y + z * z);
    u32 r = 0;
    u32 bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit != 0)
    {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return (s32)(r << shift);
}

// rmInverse16 - 16.16 reciprocal approximation (PSX: 0x800AECE8)
// Returns floor(0x7FFFFFFF / x) * 2, which approximates 2^32 / x.
// Source: C:\chan\devsys\psx\radlib\SOURCE\MATH\MULTDIV\INVERSE.C:20
inline s32 rmInverse16(s32 x)
{
    if (x == 0)
        return 0x7FFFFFFF;
    return (0x7FFFFFFF / x) * 2;
}

// rmMag2ff - fast 2D magnitude approximation (PSX: radlib MAGFAST.C)
// Returns max(|a|,|b|) + min(|a|,|b|)/4
inline s32 rmMag2ff(s32 a, s32 b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    if (b < a) {
        return a + (b >> 2);
    }
    return b + (a >> 2);
}

// rmMag3ffu - unsigned fast 3D magnitude helper (PSX: radlib MAGFAST.C)
inline s32 rmMag3ffu(u32 a1, u32 a2, u32 a3) {
    if (a2 >= a1 || a3 >= a1) {
        if (a3 >= a2) {
            if (a2 < a1) {
                return a3 + (a1 >> 2) + (a1 >> 4) + (a1 >> 5) + (a2 >> 2) + (a2 >> 5);
            }
            return a3 + (a2 >> 2) + (a2 >> 4) + (a2 >> 5) + (a1 >> 2) + (a1 >> 5);
        }
        if (a3 < a1) {
            return a2 + (a1 >> 2) + (a1 >> 4) + (a1 >> 5) + (a3 >> 2) + (a3 >> 5);
        }
        return a2 + (a3 >> 2) + (a3 >> 4) + (a3 >> 5) + (a1 >> 2) + (a1 >> 5);
    }
    if (a3 < a2) {
        return a1 + (a2 >> 2) + (a2 >> 4) + (a2 >> 5) + (a3 >> 2) + (a3 >> 5);
    }
    return a1 + (a3 >> 2) + (a3 >> 4) + (a3 >> 5) + (a2 >> 2) + (a2 >> 5);
}

// rmMag3ff - fast 3D magnitude approximation (PSX: radlib MAGFAST.C)
inline s32 rmMag3ff(s32 a1, s32 a2, s32 a3) {
    if (a1 < 0) a1 = -a1;
    if (a2 < 0) a2 = -a2;
    if (a3 < 0) a3 = -a3;
    return rmMag3ffu((u32)a1, (u32)a2, (u32)a3);
}

// rmRangedRandom - PRNG returning value in [0, range)
// PSX: 0x80078404, Source: C:\chan\devsys\psx\radlib\SOURCE\MATH\RANDOM\RANDOM0.C:48
inline u32& rmRandomSeedRef() {
    static u32 seed = 0x12345678;
    return seed;
}

inline u32 rmAdvanceRandomSeed(u32 seed) {
    const u32 a = seed ^ 0x1D872B41u;
    const u32 b = a ^ (a >> 5);
    const u32 c = a ^ (b << 27);
    return b ^ c;
}

inline u32 rmRangedRandom(u32 range) {
    u32& seed = rmRandomSeedRef();
    if (range == 0) {
        return 0;
    }

    const u32 value = seed;
    seed = rmAdvanceRandomSeed(seed);
    return value % range;
}

// rmSignedRandom17CurrentSeed - particle path helper used by PSX PARTICLE.CPP.
// Returns (seed & 0x1FFFF) - 0xFFFF from current seed, then advances the seed.
inline s32 rmSignedRandom17CurrentSeed() {
    u32& seed = rmRandomSeedRef();
    const u32 oldSeed = seed;
    seed = rmAdvanceRandomSeed(seed);
    return static_cast<s32>(oldSeed & 0x1FFFFu) - 0xFFFF;
}

// rmDiv16i - 16.16 fixed-point division (PSX: 0x8007D8B4)
// Returns PSX DIVIDE.C result (scaled long division with sign handling).
// Source: C:\chan\devsys\psx\radlib\SOURCE\MATH\MULTDIV\DIVIDE.C:30
inline s32 rmDiv16i(s32 a, s32 b) {
    s32 shift = 16;
    s32 negate = 0;

    if (b < 0) {
        b = -b;
        negate = 1;
    }

    if (a < 0) {
        a = -a;
        negate ^= 1;
    }

    while (a <= 0x1FFFFFFF) {
        if (shift == 0) {
            break;
        }
        a *= 4;
        shift -= 2;
    }

    const u32 denom = static_cast<u32>(b) >> shift;
    if (denom != 0u) {
        const s32 q = a / static_cast<s32>(denom);
        return negate ? -q : q;
    }

    if (a == 0) {
        return 0;
    }

    return negate ? static_cast<s32>(0x80000001u) : 0x7FFFFFFF;
}

// rmV3Normalize - normalize integer vector to 16.16 fixed-point unit vector
// PSX: 0x8009DA64 - calls rmMag3__Flll, rmInverse16, then rmV3Scale.
// If magnitude is zero, returns {0x10000, 0, 0}.
// Source: C:\chan\devsys\psx\radlib\SOURCE\MATH\VECTOR\VECT3D.CPP:22
inline void rmV3Normalize(LVector* out, const LVector* in) {
    s32 mag = rmMag3(in->x, in->y, in->z);
    if (mag == 0) {
        out->x = 0x10000; out->y = 0; out->z = 0;
        return;
    }
    s32 inv = rmInverse16(mag);
    out->x = (s32)(((s64)in->x * inv) >> 16);
    out->y = (s32)(((s64)in->y * inv) >> 16);
    out->z = (s32)(((s64)in->z * inv) >> 16);
}

// rmV3Dot - integer 3D dot product
inline s32 rmV3Dot(const LVector* a, const LVector* b) {
    return (s32)((((s64)a->x * (s64)b->x) >> 16)
        + (((s64)a->y * (s64)b->y) >> 16)
        + (((s64)a->z * (s64)b->z) >> 16));
}

// rmV3Scale - scale vector by 16.16 fixed-point scalar
// PSX: 0x8009346C
// Source: C:\chan\devsys\psx\radlib\SOURCE\MATH\VECTOR\VECT3D.CPP
inline void rmV3Scale(LVector* out, const LVector* in, s32 scale) {
    out->x = (s32)(((s64)in->x * (s64)scale) >> 16);
    out->y = (s32)(((s64)in->y * (s64)scale) >> 16);
    out->z = (s32)(((s64)in->z * (s64)scale) >> 16);
}

static constexpr s32 kRmSinTable[257] = {
    0, 1608, 3215, 4821, 6423, 8022, 9616, 11204, 12785, 14359, 15923, 17479, 19024, 20557, 22078, 23586,
    25079, 26557, 28020, 29465, 30893, 32302, 33692, 35061, 36409, 37736, 39039, 40319, 41575, 42806, 44011, 45189,
    46340, 47464, 48558, 49624, 50660, 51665, 52639, 53581, 54491, 55368, 56212, 57022, 57797, 58538, 59243, 59913,
    60547, 61144, 61705, 62228, 62714, 63162, 63571, 63943, 64276, 64571, 64826, 65043, 65220, 65358, 65457, 65516,
    65536, 65516, 65457, 65358, 65220, 65043, 64826, 64571, 64276, 63943, 63571, 63162, 62714, 62228, 61705, 61144,
    60547, 59913, 59243, 58538, 57797, 57022, 56212, 55368, 54491, 53581, 52639, 51665, 50660, 49624, 48558, 47464,
    46340, 45189, 44011, 42806, 41575, 40319, 39039, 37736, 36409, 35061, 33692, 32302, 30893, 29465, 28020, 26557,
    25079, 23586, 22078, 20557, 19024, 17479, 15923, 14359, 12785, 11204, 9616, 8022, 6423, 4821, 3215, 1608,
    0, -1608, -3215, -4821, -6423, -8022, -9616, -11204, -12785, -14359, -15923, -17479, -19024, -20557, -22078, -23586,
    -25079, -26557, -28020, -29465, -30893, -32302, -33692, -35061, -36409, -37736, -39039, -40319, -41575, -42806, -44011, -45189,
    -46340, -47464, -48558, -49624, -50660, -51665, -52639, -53581, -54491, -55368, -56212, -57022, -57797, -58538, -59243, -59913,
    -60547, -61144, -61705, -62228, -62714, -63162, -63571, -63943, -64276, -64571, -64826, -65043, -65220, -65358, -65457, -65516,
    -65536, -65516, -65457, -65358, -65220, -65043, -64826, -64571, -64276, -63943, -63571, -63162, -62714, -62228, -61705, -61144,
    -60547, -59913, -59243, -58538, -57797, -57022, -56212, -55368, -54491, -53581, -52639, -51665, -50660, -49624, -48558, -47464,
    -46340, -45189, -44011, -42806, -41575, -40319, -39039, -37736, -36409, -35061, -33692, -32302, -30893, -29465, -28020, -26557,
    -25079, -23586, -22078, -20557, -19024, -17479, -15923, -14359, -12785, -11204, -9616, -8022, -6423, -4821, -3215, -1608,
    0
};

static constexpr s16 kRmATanTable1[64] = {
    0, 1297, 2555, 3742, 4836, 5826, 6711, 7497, 8192, 8804, 9346, 9825, 10250, 10630, 10969, 11273,
    11547, 11796, 12021, 12227, 12415, 12587, 12746, 12892, 13028, 13153, 13270, 13379, 13481, 13576, 13665, 13749,
    13828, 13903, 13973, 14040, 14103, 14162, 14219, 14273, 14325, 14374, 14420, 14465, 14508, 14548, 14587, 14625,
    14661, 14695, 14729, 14761, 14791, 14821, 14849, 14877, 14903, 14929, 14954, 14978, 15001, 15023, 15045, 15066
};

static constexpr u16 kRmATanSlopeTable1[64] = {
    41506, 40260, 37980, 35004, 31692, 28337, 25142, 22218, 19611, 17323, 15335, 13615, 12131, 10850, 9742, 8781,
    7945, 7215, 6575, 6013, 5516, 5075, 4684, 4334, 4021, 3739, 3485, 3256, 3048, 2858, 2686, 2528,
    2384, 2251, 2129, 2016, 1912, 1816, 1727, 1644, 1566, 1495, 1427, 1365, 1306, 1251, 1199, 1150,
    1105, 1062, 1021, 983, 946, 912, 880, 849, 820, 792, 765, 740, 717, 694, 672, 651
};

static constexpr s16 kRmATanTable2[32] = {
    0, 8192, 11547, 13028, 13828, 14325, 14661, 14903, 15086, 15229, 15344, 15438, 15516, 15583, 15640, 15689,
    15732, 15771, 15805, 15835, 15862, 15887, 15910, 15930, 15949, 15967, 15983, 15997, 16011, 16024, 16036, 16047
};

static constexpr u16 kRmATanSlopeTable2[32] = {
    0, 0, 0, 0, 0, 0, 0, 0, 9143, 7335, 6013, 5019, 4251, 3647, 3163, 2769,
    2445, 2174, 1946, 1752, 1585, 1441, 1316, 1207, 1110, 1025, 949, 881, 821, 766, 717, 672
};

static constexpr s16 kRmATanTable3[16] = {
    0, 15732, 16058, 16166, 16221, 16253, 16275, 16290, 16302, 16311, 16318, 16324, 16329, 16333, 16337, 16340
};

static constexpr u16 kRmATanSlopeTable3[16] = {
    0, 41626, 13897, 6951, 4171, 2781, 1986, 1489, 1158, 927, 758, 632, 534, 458, 397, 347
};

// rmSin16 - 16.16 fixed-point sine from PSX binary angle (0..65535 = full circle)
// PSX: 0x80078364 (lookup table based)
inline s32 rmSin16(s32 angle) {
    const u16 wrappedAngle = static_cast<u16>(angle);
    const u32 index = wrappedAngle >> 8;
    const s32 base = kRmSinTable[index];
    const s32 delta = kRmSinTable[index + 1] - base;

    return base + ((delta * (wrappedAngle & 0xFF)) >> 8);
}

// rmCos16 - 16.16 fixed-point cosine from PSX binary angle
inline s32 rmCos16(s32 angle) {
    return rmSin16(angle + 0x4000);
}


// rmATan16 - fixed-point atan helper used by rmATan216
// PSX: 0x800C05CC
inline s32 rmATan16(s32 value) {
    s32 magnitude = value;
    bool negate = false;
    if (magnitude < 0) {
        negate = true;
        magnitude = -magnitude;
    }

    s32 result;
    if (magnitude > 0x7FFFF) {
        if (magnitude > 0x1FFFFF) {
            if (magnitude > 0xFFFFFF) {
                if (magnitude > 0x3FFFFFF) {
                    result = 0x4000;
                } else {
                    result = ((27 * (magnitude >> 18)) >> 7) + 0x3FCA;
                }
            } 
            else {
                const u32 index = (u32)magnitude >> 20;
                result = kRmATanTable3[index]
                    + ((static_cast<u32>(static_cast<u16>(magnitude >> 4)) * kRmATanSlopeTable3[index]) >> 23);
            }
        } 
        else {
            const u32 index = (u32)magnitude >> 16;
            result = kRmATanTable2[index]
                + ((static_cast<u32>(static_cast<u16>(magnitude)) * kRmATanSlopeTable2[index]) >> 22);
        }
    } 
    else {
        const u32 index = (u32)magnitude >> 13;
        result = kRmATanTable1[index]
            + (((magnitude & 0x1FFF) * kRmATanSlopeTable1[index]) >> 18);
    }

    return negate ? -result : result;
}


// rmATan216 - two-argument arctangent returning PSX binary angle (0..65535)
// PSX: 0x80113CF0
inline u16 rmATan216(s32 x, s32 y) {
    s32 absY = y;
    s32 quadrant = 0;
    if (x < 0) {
        quadrant = 1;
        x = -x;
    }
    if (y < 0) {
        quadrant ^= 3;
        absY = -y;
    }

    const s32 angle = rmATan16(rmDiv16i(absY, x));
    switch (quadrant) {
    case 0:
        return static_cast<u16>(angle);
    case 1:
        return static_cast<u16>(0x8000 - angle);
    case 2:
        return static_cast<u16>(angle - 0x8000);
    default:
        return static_cast<u16>(-angle);
    }
}

inline u16 rmATan216(f32 x, f32 y) {
    return rmATan216(static_cast<s32>(x), static_cast<s32>(y));
}


// Convenience: build a LookAt view matrix (used by PC improved debug cam)
// Not a PSX function - wraps the existing LookAt from core.h for Vec3.
inline Mat4 p3dLookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    return LookAt(eye.x, eye.y, eye.z, target.x, target.y, target.z, up.x, up.y, up.z);
}
