// matrix.h - Mat4 (4x4 float matrix)
// PSX MATRIX: 3x5 s16 Q12 fixed-point. PC: 4x4 float column-major.
#pragma once

#include <cmath>

using f32 = float;

#if defined(RC_PLATFORM_PSP) && defined(__psp__) && defined(__mips__) \
    && !defined(PSP_DISABLE_VFPU_MAT4)
#define P3D_VFPU_MAT4 1
#endif

struct Vec3;

// 4x4 column-major float matrix (OpenGL convention)
// Column c, row r -> m[c*4 + r]
//   col 0 = right (X basis), col 1 = up (Y basis),
//   col 2 = forward (Z basis), col 3 = translation
struct Mat4 {
    f32 m[16];

    Mat4() {
        for (int i = 0; i < 16; ++i) m[i] = 0.0f;
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    static Mat4 Identity() { return Mat4(); }

    const f32* Data() const { return m; }

    // element access: (row, col) -> m[col*4 + row]
    f32  operator()(int row, int col) const { return m[col * 4 + row]; }
    f32& operator()(int row, int col)       { return m[col * 4 + row]; }

    // column access (basis vectors)
    f32 Right(int row)   const { return m[row]; }         // col 0
    f32 Up(int row)      const { return m[4 + row]; }     // col 1
    f32 Forward(int row) const { return m[8 + row]; }     // col 2

    // translation (col 3)
    f32  GetTransX() const { return m[12]; }
    f32  GetTransY() const { return m[13]; }
    f32  GetTransZ() const { return m[14]; }
    void SetTranslation(f32 x, f32 y, f32 z) { m[12] = x; m[13] = y; m[14] = z; }

    // scale the 3x3 rotation part uniformly
    void ScaleRotation(f32 s) {
        for (int i = 0; i < 12; i++) {
            m[i] *= s;
        }
    }

    // matrix multiply
    Mat4 operator*(const Mat4& b) const;
};

// Scalar column-major product r = a * b (r must not alias a or b).
inline void p3dMat4MulScalar(const f32* a, const f32* b, f32* r) {
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            f32 sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = sum;
        }
}

#if defined(P3D_VFPU_MAT4)
// VFPU 4x4 product r = a * b of column-major matrices (4-byte aligned; the
// lv.s/sv.s loads need no 16-byte alignment). Mode picks the vmmul operand
// order and result orientation; p3dVfpuMat4Select() picks, once, the one that
// reproduces the scalar product (else the scalar product itself). It uses
// M000-M200 as scratch: GCC does not track VFPU registers, so no caller may
// hold values there across a Mat4 product (the skin/light kernels load theirs
// after any product; pspgum_vfpu keeps its matrix in M700).
template <int Mode>
inline void p3dVfpuMat4Mul(const f32* a, const f32* b, f32* r);
template <>
__attribute__((noinline)) inline void p3dVfpuMat4Mul<1>(const f32* a, const f32* b, f32* r) {
    __asm__ volatile(
        "lv.s    S000, 0(%[a])\n"
        "lv.s    S001, 4(%[a])\n"
        "lv.s    S002, 8(%[a])\n"
        "lv.s    S003, 12(%[a])\n"
        "lv.s    S010, 16(%[a])\n"
        "lv.s    S011, 20(%[a])\n"
        "lv.s    S012, 24(%[a])\n"
        "lv.s    S013, 28(%[a])\n"
        "lv.s    S020, 32(%[a])\n"
        "lv.s    S021, 36(%[a])\n"
        "lv.s    S022, 40(%[a])\n"
        "lv.s    S023, 44(%[a])\n"
        "lv.s    S030, 48(%[a])\n"
        "lv.s    S031, 52(%[a])\n"
        "lv.s    S032, 56(%[a])\n"
        "lv.s    S033, 60(%[a])\n"
        "lv.s    S100, 0(%[b])\n"
        "lv.s    S101, 4(%[b])\n"
        "lv.s    S102, 8(%[b])\n"
        "lv.s    S103, 12(%[b])\n"
        "lv.s    S110, 16(%[b])\n"
        "lv.s    S111, 20(%[b])\n"
        "lv.s    S112, 24(%[b])\n"
        "lv.s    S113, 28(%[b])\n"
        "lv.s    S120, 32(%[b])\n"
        "lv.s    S121, 36(%[b])\n"
        "lv.s    S122, 40(%[b])\n"
        "lv.s    S123, 44(%[b])\n"
        "lv.s    S130, 48(%[b])\n"
        "lv.s    S131, 52(%[b])\n"
        "lv.s    S132, 56(%[b])\n"
        "lv.s    S133, 60(%[b])\n"
        "vmmul.q M200, M000, M100\n"
        "sv.s    S200, 0(%[r])\n"
        "sv.s    S201, 4(%[r])\n"
        "sv.s    S202, 8(%[r])\n"
        "sv.s    S203, 12(%[r])\n"
        "sv.s    S210, 16(%[r])\n"
        "sv.s    S211, 20(%[r])\n"
        "sv.s    S212, 24(%[r])\n"
        "sv.s    S213, 28(%[r])\n"
        "sv.s    S220, 32(%[r])\n"
        "sv.s    S221, 36(%[r])\n"
        "sv.s    S222, 40(%[r])\n"
        "sv.s    S223, 44(%[r])\n"
        "sv.s    S230, 48(%[r])\n"
        "sv.s    S231, 52(%[r])\n"
        "sv.s    S232, 56(%[r])\n"
        "sv.s    S233, 60(%[r])\n"
        : : [a] "r"(a), [b] "r"(b), [r] "r"(r) : "memory");
}
template <>
__attribute__((noinline)) inline void p3dVfpuMat4Mul<2>(const f32* a, const f32* b, f32* r) {
    __asm__ volatile(
        "lv.s    S000, 0(%[a])\n"
        "lv.s    S001, 4(%[a])\n"
        "lv.s    S002, 8(%[a])\n"
        "lv.s    S003, 12(%[a])\n"
        "lv.s    S010, 16(%[a])\n"
        "lv.s    S011, 20(%[a])\n"
        "lv.s    S012, 24(%[a])\n"
        "lv.s    S013, 28(%[a])\n"
        "lv.s    S020, 32(%[a])\n"
        "lv.s    S021, 36(%[a])\n"
        "lv.s    S022, 40(%[a])\n"
        "lv.s    S023, 44(%[a])\n"
        "lv.s    S030, 48(%[a])\n"
        "lv.s    S031, 52(%[a])\n"
        "lv.s    S032, 56(%[a])\n"
        "lv.s    S033, 60(%[a])\n"
        "lv.s    S100, 0(%[b])\n"
        "lv.s    S101, 4(%[b])\n"
        "lv.s    S102, 8(%[b])\n"
        "lv.s    S103, 12(%[b])\n"
        "lv.s    S110, 16(%[b])\n"
        "lv.s    S111, 20(%[b])\n"
        "lv.s    S112, 24(%[b])\n"
        "lv.s    S113, 28(%[b])\n"
        "lv.s    S120, 32(%[b])\n"
        "lv.s    S121, 36(%[b])\n"
        "lv.s    S122, 40(%[b])\n"
        "lv.s    S123, 44(%[b])\n"
        "lv.s    S130, 48(%[b])\n"
        "lv.s    S131, 52(%[b])\n"
        "lv.s    S132, 56(%[b])\n"
        "lv.s    S133, 60(%[b])\n"
        "vmmul.q M200, M000, M100\n"
        "sv.s    S200, 0(%[r])\n"
        "sv.s    S210, 4(%[r])\n"
        "sv.s    S220, 8(%[r])\n"
        "sv.s    S230, 12(%[r])\n"
        "sv.s    S201, 16(%[r])\n"
        "sv.s    S211, 20(%[r])\n"
        "sv.s    S221, 24(%[r])\n"
        "sv.s    S231, 28(%[r])\n"
        "sv.s    S202, 32(%[r])\n"
        "sv.s    S212, 36(%[r])\n"
        "sv.s    S222, 40(%[r])\n"
        "sv.s    S232, 44(%[r])\n"
        "sv.s    S203, 48(%[r])\n"
        "sv.s    S213, 52(%[r])\n"
        "sv.s    S223, 56(%[r])\n"
        "sv.s    S233, 60(%[r])\n"
        : : [a] "r"(a), [b] "r"(b), [r] "r"(r) : "memory");
}
template <>
__attribute__((noinline)) inline void p3dVfpuMat4Mul<3>(const f32* a, const f32* b, f32* r) {
    __asm__ volatile(
        "lv.s    S000, 0(%[a])\n"
        "lv.s    S001, 4(%[a])\n"
        "lv.s    S002, 8(%[a])\n"
        "lv.s    S003, 12(%[a])\n"
        "lv.s    S010, 16(%[a])\n"
        "lv.s    S011, 20(%[a])\n"
        "lv.s    S012, 24(%[a])\n"
        "lv.s    S013, 28(%[a])\n"
        "lv.s    S020, 32(%[a])\n"
        "lv.s    S021, 36(%[a])\n"
        "lv.s    S022, 40(%[a])\n"
        "lv.s    S023, 44(%[a])\n"
        "lv.s    S030, 48(%[a])\n"
        "lv.s    S031, 52(%[a])\n"
        "lv.s    S032, 56(%[a])\n"
        "lv.s    S033, 60(%[a])\n"
        "lv.s    S100, 0(%[b])\n"
        "lv.s    S101, 4(%[b])\n"
        "lv.s    S102, 8(%[b])\n"
        "lv.s    S103, 12(%[b])\n"
        "lv.s    S110, 16(%[b])\n"
        "lv.s    S111, 20(%[b])\n"
        "lv.s    S112, 24(%[b])\n"
        "lv.s    S113, 28(%[b])\n"
        "lv.s    S120, 32(%[b])\n"
        "lv.s    S121, 36(%[b])\n"
        "lv.s    S122, 40(%[b])\n"
        "lv.s    S123, 44(%[b])\n"
        "lv.s    S130, 48(%[b])\n"
        "lv.s    S131, 52(%[b])\n"
        "lv.s    S132, 56(%[b])\n"
        "lv.s    S133, 60(%[b])\n"
        "vmmul.q M200, E000, M100\n"
        "sv.s    S200, 0(%[r])\n"
        "sv.s    S201, 4(%[r])\n"
        "sv.s    S202, 8(%[r])\n"
        "sv.s    S203, 12(%[r])\n"
        "sv.s    S210, 16(%[r])\n"
        "sv.s    S211, 20(%[r])\n"
        "sv.s    S212, 24(%[r])\n"
        "sv.s    S213, 28(%[r])\n"
        "sv.s    S220, 32(%[r])\n"
        "sv.s    S221, 36(%[r])\n"
        "sv.s    S222, 40(%[r])\n"
        "sv.s    S223, 44(%[r])\n"
        "sv.s    S230, 48(%[r])\n"
        "sv.s    S231, 52(%[r])\n"
        "sv.s    S232, 56(%[r])\n"
        "sv.s    S233, 60(%[r])\n"
        : : [a] "r"(a), [b] "r"(b), [r] "r"(r) : "memory");
}
template <>
__attribute__((noinline)) inline void p3dVfpuMat4Mul<4>(const f32* a, const f32* b, f32* r) {
    __asm__ volatile(
        "lv.s    S000, 0(%[a])\n"
        "lv.s    S001, 4(%[a])\n"
        "lv.s    S002, 8(%[a])\n"
        "lv.s    S003, 12(%[a])\n"
        "lv.s    S010, 16(%[a])\n"
        "lv.s    S011, 20(%[a])\n"
        "lv.s    S012, 24(%[a])\n"
        "lv.s    S013, 28(%[a])\n"
        "lv.s    S020, 32(%[a])\n"
        "lv.s    S021, 36(%[a])\n"
        "lv.s    S022, 40(%[a])\n"
        "lv.s    S023, 44(%[a])\n"
        "lv.s    S030, 48(%[a])\n"
        "lv.s    S031, 52(%[a])\n"
        "lv.s    S032, 56(%[a])\n"
        "lv.s    S033, 60(%[a])\n"
        "lv.s    S100, 0(%[b])\n"
        "lv.s    S101, 4(%[b])\n"
        "lv.s    S102, 8(%[b])\n"
        "lv.s    S103, 12(%[b])\n"
        "lv.s    S110, 16(%[b])\n"
        "lv.s    S111, 20(%[b])\n"
        "lv.s    S112, 24(%[b])\n"
        "lv.s    S113, 28(%[b])\n"
        "lv.s    S120, 32(%[b])\n"
        "lv.s    S121, 36(%[b])\n"
        "lv.s    S122, 40(%[b])\n"
        "lv.s    S123, 44(%[b])\n"
        "lv.s    S130, 48(%[b])\n"
        "lv.s    S131, 52(%[b])\n"
        "lv.s    S132, 56(%[b])\n"
        "lv.s    S133, 60(%[b])\n"
        "vmmul.q M200, E000, M100\n"
        "sv.s    S200, 0(%[r])\n"
        "sv.s    S210, 4(%[r])\n"
        "sv.s    S220, 8(%[r])\n"
        "sv.s    S230, 12(%[r])\n"
        "sv.s    S201, 16(%[r])\n"
        "sv.s    S211, 20(%[r])\n"
        "sv.s    S221, 24(%[r])\n"
        "sv.s    S231, 28(%[r])\n"
        "sv.s    S202, 32(%[r])\n"
        "sv.s    S212, 36(%[r])\n"
        "sv.s    S222, 40(%[r])\n"
        "sv.s    S232, 44(%[r])\n"
        "sv.s    S203, 48(%[r])\n"
        "sv.s    S213, 52(%[r])\n"
        "sv.s    S223, 56(%[r])\n"
        "sv.s    S233, 60(%[r])\n"
        : : [a] "r"(a), [b] "r"(b), [r] "r"(r) : "memory");
}
template <>
__attribute__((noinline)) inline void p3dVfpuMat4Mul<5>(const f32* a, const f32* b, f32* r) {
    __asm__ volatile(
        "lv.s    S000, 0(%[a])\n"
        "lv.s    S001, 4(%[a])\n"
        "lv.s    S002, 8(%[a])\n"
        "lv.s    S003, 12(%[a])\n"
        "lv.s    S010, 16(%[a])\n"
        "lv.s    S011, 20(%[a])\n"
        "lv.s    S012, 24(%[a])\n"
        "lv.s    S013, 28(%[a])\n"
        "lv.s    S020, 32(%[a])\n"
        "lv.s    S021, 36(%[a])\n"
        "lv.s    S022, 40(%[a])\n"
        "lv.s    S023, 44(%[a])\n"
        "lv.s    S030, 48(%[a])\n"
        "lv.s    S031, 52(%[a])\n"
        "lv.s    S032, 56(%[a])\n"
        "lv.s    S033, 60(%[a])\n"
        "lv.s    S100, 0(%[b])\n"
        "lv.s    S101, 4(%[b])\n"
        "lv.s    S102, 8(%[b])\n"
        "lv.s    S103, 12(%[b])\n"
        "lv.s    S110, 16(%[b])\n"
        "lv.s    S111, 20(%[b])\n"
        "lv.s    S112, 24(%[b])\n"
        "lv.s    S113, 28(%[b])\n"
        "lv.s    S120, 32(%[b])\n"
        "lv.s    S121, 36(%[b])\n"
        "lv.s    S122, 40(%[b])\n"
        "lv.s    S123, 44(%[b])\n"
        "lv.s    S130, 48(%[b])\n"
        "lv.s    S131, 52(%[b])\n"
        "lv.s    S132, 56(%[b])\n"
        "lv.s    S133, 60(%[b])\n"
        "vmmul.q M200, M100, M000\n"
        "sv.s    S200, 0(%[r])\n"
        "sv.s    S201, 4(%[r])\n"
        "sv.s    S202, 8(%[r])\n"
        "sv.s    S203, 12(%[r])\n"
        "sv.s    S210, 16(%[r])\n"
        "sv.s    S211, 20(%[r])\n"
        "sv.s    S212, 24(%[r])\n"
        "sv.s    S213, 28(%[r])\n"
        "sv.s    S220, 32(%[r])\n"
        "sv.s    S221, 36(%[r])\n"
        "sv.s    S222, 40(%[r])\n"
        "sv.s    S223, 44(%[r])\n"
        "sv.s    S230, 48(%[r])\n"
        "sv.s    S231, 52(%[r])\n"
        "sv.s    S232, 56(%[r])\n"
        "sv.s    S233, 60(%[r])\n"
        : : [a] "r"(a), [b] "r"(b), [r] "r"(r) : "memory");
}
template <>
__attribute__((noinline)) inline void p3dVfpuMat4Mul<6>(const f32* a, const f32* b, f32* r) {
    __asm__ volatile(
        "lv.s    S000, 0(%[a])\n"
        "lv.s    S001, 4(%[a])\n"
        "lv.s    S002, 8(%[a])\n"
        "lv.s    S003, 12(%[a])\n"
        "lv.s    S010, 16(%[a])\n"
        "lv.s    S011, 20(%[a])\n"
        "lv.s    S012, 24(%[a])\n"
        "lv.s    S013, 28(%[a])\n"
        "lv.s    S020, 32(%[a])\n"
        "lv.s    S021, 36(%[a])\n"
        "lv.s    S022, 40(%[a])\n"
        "lv.s    S023, 44(%[a])\n"
        "lv.s    S030, 48(%[a])\n"
        "lv.s    S031, 52(%[a])\n"
        "lv.s    S032, 56(%[a])\n"
        "lv.s    S033, 60(%[a])\n"
        "lv.s    S100, 0(%[b])\n"
        "lv.s    S101, 4(%[b])\n"
        "lv.s    S102, 8(%[b])\n"
        "lv.s    S103, 12(%[b])\n"
        "lv.s    S110, 16(%[b])\n"
        "lv.s    S111, 20(%[b])\n"
        "lv.s    S112, 24(%[b])\n"
        "lv.s    S113, 28(%[b])\n"
        "lv.s    S120, 32(%[b])\n"
        "lv.s    S121, 36(%[b])\n"
        "lv.s    S122, 40(%[b])\n"
        "lv.s    S123, 44(%[b])\n"
        "lv.s    S130, 48(%[b])\n"
        "lv.s    S131, 52(%[b])\n"
        "lv.s    S132, 56(%[b])\n"
        "lv.s    S133, 60(%[b])\n"
        "vmmul.q M200, M100, M000\n"
        "sv.s    S200, 0(%[r])\n"
        "sv.s    S210, 4(%[r])\n"
        "sv.s    S220, 8(%[r])\n"
        "sv.s    S230, 12(%[r])\n"
        "sv.s    S201, 16(%[r])\n"
        "sv.s    S211, 20(%[r])\n"
        "sv.s    S221, 24(%[r])\n"
        "sv.s    S231, 28(%[r])\n"
        "sv.s    S202, 32(%[r])\n"
        "sv.s    S212, 36(%[r])\n"
        "sv.s    S222, 40(%[r])\n"
        "sv.s    S232, 44(%[r])\n"
        "sv.s    S203, 48(%[r])\n"
        "sv.s    S213, 52(%[r])\n"
        "sv.s    S223, 56(%[r])\n"
        "sv.s    S233, 60(%[r])\n"
        : : [a] "r"(a), [b] "r"(b), [r] "r"(r) : "memory");
}
template <>
__attribute__((noinline)) inline void p3dVfpuMat4Mul<7>(const f32* a, const f32* b, f32* r) {
    __asm__ volatile(
        "lv.s    S000, 0(%[a])\n"
        "lv.s    S001, 4(%[a])\n"
        "lv.s    S002, 8(%[a])\n"
        "lv.s    S003, 12(%[a])\n"
        "lv.s    S010, 16(%[a])\n"
        "lv.s    S011, 20(%[a])\n"
        "lv.s    S012, 24(%[a])\n"
        "lv.s    S013, 28(%[a])\n"
        "lv.s    S020, 32(%[a])\n"
        "lv.s    S021, 36(%[a])\n"
        "lv.s    S022, 40(%[a])\n"
        "lv.s    S023, 44(%[a])\n"
        "lv.s    S030, 48(%[a])\n"
        "lv.s    S031, 52(%[a])\n"
        "lv.s    S032, 56(%[a])\n"
        "lv.s    S033, 60(%[a])\n"
        "lv.s    S100, 0(%[b])\n"
        "lv.s    S101, 4(%[b])\n"
        "lv.s    S102, 8(%[b])\n"
        "lv.s    S103, 12(%[b])\n"
        "lv.s    S110, 16(%[b])\n"
        "lv.s    S111, 20(%[b])\n"
        "lv.s    S112, 24(%[b])\n"
        "lv.s    S113, 28(%[b])\n"
        "lv.s    S120, 32(%[b])\n"
        "lv.s    S121, 36(%[b])\n"
        "lv.s    S122, 40(%[b])\n"
        "lv.s    S123, 44(%[b])\n"
        "lv.s    S130, 48(%[b])\n"
        "lv.s    S131, 52(%[b])\n"
        "lv.s    S132, 56(%[b])\n"
        "lv.s    S133, 60(%[b])\n"
        "vmmul.q M200, E100, M000\n"
        "sv.s    S200, 0(%[r])\n"
        "sv.s    S201, 4(%[r])\n"
        "sv.s    S202, 8(%[r])\n"
        "sv.s    S203, 12(%[r])\n"
        "sv.s    S210, 16(%[r])\n"
        "sv.s    S211, 20(%[r])\n"
        "sv.s    S212, 24(%[r])\n"
        "sv.s    S213, 28(%[r])\n"
        "sv.s    S220, 32(%[r])\n"
        "sv.s    S221, 36(%[r])\n"
        "sv.s    S222, 40(%[r])\n"
        "sv.s    S223, 44(%[r])\n"
        "sv.s    S230, 48(%[r])\n"
        "sv.s    S231, 52(%[r])\n"
        "sv.s    S232, 56(%[r])\n"
        "sv.s    S233, 60(%[r])\n"
        : : [a] "r"(a), [b] "r"(b), [r] "r"(r) : "memory");
}
template <>
__attribute__((noinline)) inline void p3dVfpuMat4Mul<8>(const f32* a, const f32* b, f32* r) {
    __asm__ volatile(
        "lv.s    S000, 0(%[a])\n"
        "lv.s    S001, 4(%[a])\n"
        "lv.s    S002, 8(%[a])\n"
        "lv.s    S003, 12(%[a])\n"
        "lv.s    S010, 16(%[a])\n"
        "lv.s    S011, 20(%[a])\n"
        "lv.s    S012, 24(%[a])\n"
        "lv.s    S013, 28(%[a])\n"
        "lv.s    S020, 32(%[a])\n"
        "lv.s    S021, 36(%[a])\n"
        "lv.s    S022, 40(%[a])\n"
        "lv.s    S023, 44(%[a])\n"
        "lv.s    S030, 48(%[a])\n"
        "lv.s    S031, 52(%[a])\n"
        "lv.s    S032, 56(%[a])\n"
        "lv.s    S033, 60(%[a])\n"
        "lv.s    S100, 0(%[b])\n"
        "lv.s    S101, 4(%[b])\n"
        "lv.s    S102, 8(%[b])\n"
        "lv.s    S103, 12(%[b])\n"
        "lv.s    S110, 16(%[b])\n"
        "lv.s    S111, 20(%[b])\n"
        "lv.s    S112, 24(%[b])\n"
        "lv.s    S113, 28(%[b])\n"
        "lv.s    S120, 32(%[b])\n"
        "lv.s    S121, 36(%[b])\n"
        "lv.s    S122, 40(%[b])\n"
        "lv.s    S123, 44(%[b])\n"
        "lv.s    S130, 48(%[b])\n"
        "lv.s    S131, 52(%[b])\n"
        "lv.s    S132, 56(%[b])\n"
        "lv.s    S133, 60(%[b])\n"
        "vmmul.q M200, E100, M000\n"
        "sv.s    S200, 0(%[r])\n"
        "sv.s    S210, 4(%[r])\n"
        "sv.s    S220, 8(%[r])\n"
        "sv.s    S230, 12(%[r])\n"
        "sv.s    S201, 16(%[r])\n"
        "sv.s    S211, 20(%[r])\n"
        "sv.s    S221, 24(%[r])\n"
        "sv.s    S231, 28(%[r])\n"
        "sv.s    S202, 32(%[r])\n"
        "sv.s    S212, 36(%[r])\n"
        "sv.s    S222, 40(%[r])\n"
        "sv.s    S232, 44(%[r])\n"
        "sv.s    S203, 48(%[r])\n"
        "sv.s    S213, 52(%[r])\n"
        "sv.s    S223, 56(%[r])\n"
        "sv.s    S233, 60(%[r])\n"
        : : [a] "r"(a), [b] "r"(b), [r] "r"(r) : "memory");
}

using P3dMat4MulFn = void (*)(const f32* a, const f32* b, f32* r);
// Candidate kept by p3dVfpuMat4Select (1-8), 0 for the scalar product.
inline int g_p3dVfpuMat4Mode = -1;

__attribute__((noinline)) inline P3dMat4MulFn p3dVfpuMat4Select() {
    constexpr P3dMat4MulFn kModes[8] = {
        &p3dVfpuMat4Mul<1>, &p3dVfpuMat4Mul<2>, &p3dVfpuMat4Mul<3>, &p3dVfpuMat4Mul<4>,
        &p3dVfpuMat4Mul<5>, &p3dVfpuMat4Mul<6>, &p3dVfpuMat4Mul<7>, &p3dVfpuMat4Mul<8>,
    };
    Mat4 a;
    Mat4 b;
    for (int i = 0; i < 16; ++i) {
        a.m[i] = static_cast<f32>((i * 7 + 3) % 11) - 4.5f;
        b.m[i] = static_cast<f32>((i * 5 + 1) % 13) * 0.25f - 1.0f;
    }
    f32 expected[16];
    p3dMat4MulScalar(a.m, b.m, expected);
    for (int mode = 0; mode < 8; ++mode) {
        const P3dMat4MulFn candidate = kModes[mode];
        f32 actual[16];
        candidate(a.m, b.m, actual);
        bool same = true;
        for (int i = 0; i < 16 && same; ++i) {
            const f32 tolerance = 1.0e-4f * (1.0f + std::fabs(expected[i]));
            same = std::fabs(actual[i] - expected[i]) <= tolerance;
        }
        if (same) {
            g_p3dVfpuMat4Mode = mode + 1;
            return candidate;
        }
    }
    g_p3dVfpuMat4Mode = 0;
    return &p3dMat4MulScalar;
}

// The product used by Mat4::operator*: chosen on first use.
inline P3dMat4MulFn g_p3dMat4Mul = nullptr;
#endif

inline Mat4 Mat4::operator*(const Mat4& b) const {
    Mat4 r;
#if defined(P3D_VFPU_MAT4)
    // A skeleton build multiplies once or twice per joint; the scalar loop
    // is bound by FPU latency (~10x the VFPU product).
    if (!g_p3dMat4Mul) g_p3dMat4Mul = p3dVfpuMat4Select();
    g_p3dMat4Mul(m, b.m, r.m);
#else
    p3dMat4MulScalar(m, b.m, r.m);
#endif
    return r;
}

// r = a * b written straight into r (no returned temporary): the same
// product as operator*. r must not alias a or b.
inline void p3dMat4MulInto(const Mat4& a, const Mat4& b, Mat4& r) {
#if defined(P3D_VFPU_MAT4)
    if (!g_p3dMat4Mul) g_p3dMat4Mul = p3dVfpuMat4Select();
    g_p3dMat4Mul(a.m, b.m, r.m);
#else
    p3dMat4MulScalar(a.m, b.m, r.m);
#endif
}

// transform point (rotation + translation)
inline void Mat4TransformPoint(const Mat4& m, f32 ix, f32 iy, f32 iz,
                               f32& ox, f32& oy, f32& oz) {
    ox = m.m[0] * ix + m.m[4] * iy + m.m[8]  * iz + m.m[12];
    oy = m.m[1] * ix + m.m[5] * iy + m.m[9]  * iz + m.m[13];
    oz = m.m[2] * ix + m.m[6] * iy + m.m[10] * iz + m.m[14];
}

// transform vector (rotation only, no translation)
inline void Mat4TransformDir(const Mat4& m, f32 ix, f32 iy, f32 iz,
                             f32& ox, f32& oy, f32& oz) {
    ox = m.m[0] * ix + m.m[4] * iy + m.m[8]  * iz;
    oy = m.m[1] * ix + m.m[5] * iy + m.m[9]  * iz;
    oz = m.m[2] * ix + m.m[6] * iy + m.m[10] * iz;
}

inline Mat4 Ortho(f32 left, f32 right, f32 bottom, f32 top, f32 near = -1.0f, f32 far = 1.0f) {
    Mat4 o;
    o.m[0] = 2.0f / (right - left);
    o.m[5] = 2.0f / (top - bottom);
    o.m[10] = -2.0f / (far - near);
    o.m[12] = -(right + left) / (right - left);
    o.m[13] = -(top + bottom) / (top - bottom);
    o.m[14] = -(far + near) / (far - near);
    o.m[15] = 1.0f;
    return o;
}

inline Mat4 Perspective(f32 fovY, f32 aspect, f32 near, f32 far) {
    f32 tanHalf = 1.0f / std::tan(fovY * 0.5f);
    Mat4 p;
    p.m[0] = tanHalf / aspect;
    p.m[5] = tanHalf;
    p.m[10] = -(far + near) / (far - near);
    p.m[11] = -1.0f;
    p.m[14] = -(2.0f * far * near) / (far - near);
    p.m[15] = 0.0f;
    return p;
}

inline Mat4 PerspectiveReversedZ(f32 fovY, f32 aspect, f32 near, f32 far) {
    f32 tanHalf = 1.0f / std::tan(fovY * 0.5f);
    Mat4 p;
    p.m[0] = tanHalf / aspect;
    p.m[5] = tanHalf;
    p.m[10] = near / (far - near);
    p.m[11] = -1.0f;
    p.m[14] = (far * near) / (far - near);
    p.m[15] = 0.0f;
    return p;
}

inline Mat4 PerspectiveReversedZTangents(f32 tanHalfX, f32 tanHalfY, f32 near, f32 far) {
    if (tanHalfX < 0.0001f) tanHalfX = 0.0001f;
    if (tanHalfY < 0.0001f) tanHalfY = 0.0001f;

    Mat4 p;
    p.m[0] = 1.0f / tanHalfX;
    p.m[5] = 1.0f / tanHalfY;
    p.m[10] = near / (far - near);
    p.m[11] = -1.0f;
    p.m[14] = (far * near) / (far - near);
    p.m[15] = 0.0f;
    return p;
}

inline Mat4 LookAt(f32 ex, f32 ey, f32 ez, f32 cx, f32 cy, f32 cz, f32 ux, f32 uy, f32 uz) {
    // forward = normalize(eye - center)
    f32 fx = ex - cx, fy = ey - cy, fz = ez - cz;
    f32 fl = 1.0f / std::sqrt(fx * fx + fy * fy + fz * fz);
    fx *= fl; fy *= fl; fz *= fl;
    // right = normalize(up x forward)
    f32 rx = uy * fz - uz * fy, ry = uz * fx - ux * fz, rz = ux * fy - uy * fx;
    f32 rl = 1.0f / std::sqrt(rx * rx + ry * ry + rz * rz);
    rx *= rl; ry *= rl; rz *= rl;
    // up = forward x right
    f32 upx = fy * rz - fz * ry, upy = fz * rx - fx * rz, upz = fx * ry - fy * rx;
    Mat4 m;
    m.m[0] = rx;  m.m[4] = ry;  m.m[8] = rz;  m.m[12] = -(rx * ex + ry * ey + rz * ez);
    m.m[1] = upx; m.m[5] = upy; m.m[9] = upz; m.m[13] = -(upx * ex + upy * ey + upz * ez);
    m.m[2] = fx;  m.m[6] = fy;  m.m[10] = fz;  m.m[14] = -(fx * ex + fy * ey + fz * ez);
    m.m[3] = 0;   m.m[7] = 0;   m.m[11] = 0;   m.m[15] = 1.0f;
    return m;
}
