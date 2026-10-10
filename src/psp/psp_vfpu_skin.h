#pragma once

// VFPU kernels for the per-frame character skinning in DrawableSTree::Display
// (PSP only; -DPSP_DISABLE_VFPU_SKIN restores the scalar loops for A/B).
//
// Contract with the scalar path:
// - Lighting (the RP_XformVertsLitCBF_CL work) is bit-exact. The joint normal
//   rotation is truncated to an integer, so it uses VFPU vscl/vadd in the same
//   order as Mat4TransformDir (single IEEE multiply/add each, no vdot), and
//   the light dot products/accumulation stay in exact integer arithmetic.
// - Vertex colours are bit-exact: every float step is exactly representable.
// - Positions use vhtfm4.q, whose internal dot product may differ from the
//   sequential FPU sum in the last bits (an emulated difference far below a
//   texel/pixel). The first eligible draw self-checks against the scalar path
//   and keeps the scalar path if any of these contracts fails.
//
// Register map. libpspgum_vfpu keeps the current GUM matrix resident in M3
// (sceGumLoadMatrix/sceGumUpdateMatrix), so M3 is never touched here. M0/M1
// are GUM temporaries and are only relied on inside a single kernel run that
// makes no GUM call.
//   M0 joint matrix (columns)          M1 per-vertex temporaries
//   M2 vertex constants (vectors)      M4 vertex constants (scalars)
//   M5 lighting matrix (3 columns)     M6 normal rotation temporaries
//   M7 attribute copy (u, v, tpage, cba)
//
// The portable reference functions below are what the host tests compare
// with the production scalar code; the PSP kernels implement the same steps.

#include "p3d/matrix.h"
#include "psp/psp_scratchpad.h"
#include <cstddef>
#include <cstdint>
#include <new>

namespace PspVfpuSkin {

// --- SkinVertex/output layout the kernels read (static_asserted by model.cpp)
constexpr unsigned kVertexStrideBytes = 48;
constexpr unsigned kVertexColourOffset = 12;   // r, g, b
constexpr unsigned kVertexAttribOffset = 24;   // u, v, tpage, cba
constexpr unsigned kOutputFloats = 10;

// lv.q/sv.q need 16-byte aligned addresses.
template <typename T>
struct Align16Allocator {
    using value_type = T;
    Align16Allocator() noexcept = default;
    template <typename U>
    Align16Allocator(const Align16Allocator<U>&) noexcept {}
    T* allocate(std::size_t count) {
        return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{16}));
    }
    void deallocate(T* pointer, std::size_t) noexcept {
        ::operator delete(pointer, std::align_val_t{16});
    }
    template <typename U>
    bool operator==(const Align16Allocator<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const Align16Allocator<U>&) const noexcept { return false; }
};

// PSX RPSTREECOL normal table (gen/geometry.cpp s_rpStreeNormals) as float
// quads for lv.q. Exact conversions of the s16 entries; the lighting test
// compares every entry against ComputePsxLitColourFromNormalIndex.
alignas(16) inline constexpr float kNormals[256][4] = {
    {     0.0f,      0.0f,  -4096.0f, 0.0f}, {     0.0f,      0.0f,   4096.0f, 0.0f},
    {     0.0f,  -4096.0f,      0.0f, 0.0f}, {     0.0f,   4096.0f,      0.0f, 0.0f},
    { -4096.0f,      0.0f,      0.0f, 0.0f}, {  4096.0f,      0.0f,      0.0f, 0.0f},
    {  3664.0f,      0.0f,   1832.0f, 0.0f}, {  1132.0f,   3484.0f,   1832.0f, 0.0f},
    { -2964.0f,   2153.0f,   1832.0f, 0.0f}, { -2964.0f,  -2153.0f,   1832.0f, 0.0f},
    {  1132.0f,  -3484.0f,   1832.0f, 0.0f}, {  2964.0f,   2153.0f,  -1832.0f, 0.0f},
    { -1132.0f,   3484.0f,  -1832.0f, 0.0f}, { -3664.0f,      0.0f,  -1832.0f, 0.0f},
    { -1132.0f,  -3484.0f,  -1832.0f, 0.0f}, {  2964.0f,  -2153.0f,  -1832.0f, 0.0f},
    {   900.0f,      0.0f,   3996.0f, 0.0f}, {  1755.0f,      0.0f,   3701.0f, 0.0f},
    {  2525.0f,      0.0f,   3225.0f, 0.0f}, {  3172.0f,      0.0f,   2592.0f, 0.0f},
    {   278.0f,    856.0f,   3996.0f, 0.0f}, {   542.0f,   1669.0f,   3701.0f, 0.0f},
    {   780.0f,   2402.0f,   3225.0f, 0.0f}, {   980.0f,   3017.0f,   2592.0f, 0.0f},
    {  -728.0f,    529.0f,   3996.0f, 0.0f}, { -1420.0f,   1032.0f,   3701.0f, 0.0f},
    { -2043.0f,   1484.0f,   3225.0f, 0.0f}, { -2566.0f,   1864.0f,   2592.0f, 0.0f},
    {  -728.0f,   -529.0f,   3996.0f, 0.0f}, { -1420.0f,  -1032.0f,   3701.0f, 0.0f},
    { -2043.0f,  -1484.0f,   3225.0f, 0.0f}, { -2566.0f,  -1864.0f,   2592.0f, 0.0f},
    {   278.0f,   -856.0f,   3996.0f, 0.0f}, {   542.0f,  -1669.0f,   3701.0f, 0.0f},
    {   780.0f,  -2402.0f,   3225.0f, 0.0f}, {   980.0f,  -3017.0f,   2592.0f, 0.0f},
    {  3450.0f,    856.0f,   2036.0f, 0.0f}, {  3068.0f,   1669.0f,   2140.0f, 0.0f},
    {  2536.0f,   2402.0f,   2140.0f, 0.0f}, {  1880.0f,   3017.0f,   2036.0f, 0.0f},
    {   252.0f,   3545.0f,   2036.0f, 0.0f}, {  -640.0f,   3433.0f,   2140.0f, 0.0f},
    { -1501.0f,   3154.0f,   2140.0f, 0.0f}, { -2288.0f,   2720.0f,   2036.0f, 0.0f},
    { -3294.0f,   1336.0f,   2036.0f, 0.0f}, { -3463.0f,    453.0f,   2140.0f, 0.0f},
    { -3463.0f,   -453.0f,   2140.0f, 0.0f}, { -3294.0f,  -1336.0f,   2036.0f, 0.0f},
    { -2288.0f,  -2720.0f,   2036.0f, 0.0f}, { -1501.0f,  -3154.0f,   2140.0f, 0.0f},
    {  -640.0f,  -3433.0f,   2140.0f, 0.0f}, {   252.0f,  -3545.0f,   2036.0f, 0.0f},
    {  1880.0f,  -3017.0f,   2036.0f, 0.0f}, {  2536.0f,  -2402.0f,   2140.0f, 0.0f},
    {  3068.0f,  -1669.0f,   2140.0f, 0.0f}, {  3450.0f,   -856.0f,   2036.0f, 0.0f},
    {  3900.0f,    529.0f,   1136.0f, 0.0f}, {  3945.0f,   1032.0f,    385.0f, 0.0f},
    {  3798.0f,   1484.0f,   -385.0f, 0.0f}, {  3466.0f,   1864.0f,  -1136.0f, 0.0f},
    {   702.0f,   3872.0f,   1136.0f, 0.0f}, {   238.0f,   4071.0f,    385.0f, 0.0f},
    {  -238.0f,   4071.0f,   -385.0f, 0.0f}, {  -702.0f,   3872.0f,  -1136.0f, 0.0f},
    { -3466.0f,   1864.0f,   1136.0f, 0.0f}, { -3798.0f,   1484.0f,    385.0f, 0.0f},
    { -3945.0f,   1032.0f,   -385.0f, 0.0f}, { -3900.0f,    529.0f,  -1136.0f, 0.0f},
    { -2844.0f,  -2720.0f,   1136.0f, 0.0f}, { -2585.0f,  -3154.0f,    385.0f, 0.0f},
    { -2200.0f,  -3433.0f,   -385.0f, 0.0f}, { -1708.0f,  -3545.0f,  -1136.0f, 0.0f},
    {  1708.0f,  -3545.0f,   1136.0f, 0.0f}, {  2200.0f,  -3433.0f,    385.0f, 0.0f},
    {  2585.0f,  -3154.0f,   -385.0f, 0.0f}, {  2844.0f,  -2720.0f,  -1136.0f, 0.0f},
    {  3900.0f,   -529.0f,   1136.0f, 0.0f}, {  3945.0f,  -1032.0f,    385.0f, 0.0f},
    {  3798.0f,  -1484.0f,   -385.0f, 0.0f}, {  3466.0f,  -1864.0f,  -1136.0f, 0.0f},
    {  1708.0f,   3545.0f,   1136.0f, 0.0f}, {  2200.0f,   3433.0f,    385.0f, 0.0f},
    {  2585.0f,   3154.0f,   -385.0f, 0.0f}, {  2844.0f,   2720.0f,  -1136.0f, 0.0f},
    { -2844.0f,   2720.0f,   1136.0f, 0.0f}, { -2585.0f,   3154.0f,    385.0f, 0.0f},
    { -2200.0f,   3433.0f,   -385.0f, 0.0f}, { -1708.0f,   3545.0f,  -1136.0f, 0.0f},
    { -3466.0f,  -1864.0f,   1136.0f, 0.0f}, { -3798.0f,  -1484.0f,    385.0f, 0.0f},
    { -3945.0f,  -1032.0f,   -385.0f, 0.0f}, { -3900.0f,   -529.0f,  -1136.0f, 0.0f},
    {   702.0f,  -3872.0f,   1136.0f, 0.0f}, {   238.0f,  -4071.0f,    385.0f, 0.0f},
    {  -238.0f,  -4071.0f,   -385.0f, 0.0f}, {  -702.0f,  -3872.0f,  -1136.0f, 0.0f},
    {  2288.0f,   2720.0f,  -2036.0f, 0.0f}, {  1501.0f,   3154.0f,  -2140.0f, 0.0f},
    {   640.0f,   3433.0f,  -2140.0f, 0.0f}, {  -252.0f,   3545.0f,  -2036.0f, 0.0f},
    { -1880.0f,   3017.0f,  -2036.0f, 0.0f}, { -2536.0f,   2402.0f,  -2140.0f, 0.0f},
    { -3068.0f,   1669.0f,  -2140.0f, 0.0f}, { -3450.0f,    856.0f,  -2036.0f, 0.0f},
    { -3450.0f,   -856.0f,  -2036.0f, 0.0f}, { -3068.0f,  -1669.0f,  -2140.0f, 0.0f},
    { -2536.0f,  -2402.0f,  -2140.0f, 0.0f}, { -1880.0f,  -3017.0f,  -2036.0f, 0.0f},
    {  -252.0f,  -3545.0f,  -2036.0f, 0.0f}, {   640.0f,  -3433.0f,  -2140.0f, 0.0f},
    {  1501.0f,  -3154.0f,  -2140.0f, 0.0f}, {  2288.0f,  -2720.0f,  -2036.0f, 0.0f},
    {  3294.0f,  -1336.0f,  -2036.0f, 0.0f}, {  3463.0f,   -453.0f,  -2140.0f, 0.0f},
    {  3463.0f,    453.0f,  -2140.0f, 0.0f}, {  3294.0f,   1336.0f,  -2036.0f, 0.0f},
    {   728.0f,    529.0f,  -3996.0f, 0.0f}, {  1420.0f,   1032.0f,  -3701.0f, 0.0f},
    {  2043.0f,   1484.0f,  -3225.0f, 0.0f}, {  2566.0f,   1864.0f,  -2592.0f, 0.0f},
    {  -278.0f,    856.0f,  -3996.0f, 0.0f}, {  -542.0f,   1669.0f,  -3701.0f, 0.0f},
    {  -780.0f,   2402.0f,  -3225.0f, 0.0f}, {  -980.0f,   3017.0f,  -2592.0f, 0.0f},
    {  -900.0f,      0.0f,  -3996.0f, 0.0f}, { -1755.0f,      0.0f,  -3701.0f, 0.0f},
    { -2525.0f,      0.0f,  -3225.0f, 0.0f}, { -3172.0f,      0.0f,  -2592.0f, 0.0f},
    {  -278.0f,   -856.0f,  -3996.0f, 0.0f}, {  -542.0f,  -1669.0f,  -3701.0f, 0.0f},
    {  -780.0f,  -2402.0f,  -3225.0f, 0.0f}, {  -980.0f,  -3017.0f,  -2592.0f, 0.0f},
    {   728.0f,   -529.0f,  -3996.0f, 0.0f}, {  1420.0f,  -1032.0f,  -3701.0f, 0.0f},
    {  2043.0f,  -1484.0f,  -3225.0f, 0.0f}, {  2566.0f,  -1864.0f,  -2592.0f, 0.0f},
    {  1187.0f,    862.0f,   3824.0f, 0.0f}, {  2057.0f,    870.0f,   3434.0f, 0.0f},
    {  1463.0f,   1687.0f,   3434.0f, 0.0f}, {  2830.0f,    871.0f,   2830.0f, 0.0f},
    {  2331.0f,   1694.0f,   2911.0f, 0.0f}, {  1703.0f,   2423.0f,   2830.0f, 0.0f},
    {  -453.0f,   1396.0f,   3824.0f, 0.0f}, {  -192.0f,   2225.0f,   3434.0f, 0.0f},
    { -1152.0f,   1913.0f,   3434.0f, 0.0f}, {    46.0f,   2961.0f,   2830.0f, 0.0f},
    {  -891.0f,   2741.0f,   2911.0f, 0.0f}, { -1778.0f,   2368.0f,   2830.0f, 0.0f},
    { -1467.0f,      0.0f,   3824.0f, 0.0f}, { -2175.0f,    505.0f,   3434.0f, 0.0f},
    { -2175.0f,   -505.0f,   3434.0f, 0.0f}, { -2802.0f,    959.0f,   2830.0f, 0.0f},
    { -2882.0f,      0.0f,   2911.0f, 0.0f}, { -2802.0f,   -959.0f,   2830.0f, 0.0f},
    {  -453.0f,  -1396.0f,   3824.0f, 0.0f}, { -1152.0f,  -1913.0f,   3434.0f, 0.0f},
    {  -192.0f,  -2225.0f,   3434.0f, 0.0f}, { -1778.0f,  -2368.0f,   2830.0f, 0.0f},
    {  -891.0f,  -2741.0f,   2911.0f, 0.0f}, {    46.0f,  -2961.0f,   2830.0f, 0.0f},
    {  1187.0f,   -862.0f,   3824.0f, 0.0f}, {  1463.0f,  -1687.0f,   3434.0f, 0.0f},
    {  2057.0f,   -870.0f,   3434.0f, 0.0f}, {  1703.0f,  -2423.0f,   2830.0f, 0.0f},
    {  2331.0f,  -1694.0f,   2911.0f, 0.0f}, {  2830.0f,   -871.0f,   2830.0f, 0.0f},
    {  4077.0f,      0.0f,    398.0f, 0.0f}, {  4044.0f,   -505.0f,   -410.0f, 0.0f},
    {  4044.0f,    505.0f,   -410.0f, 0.0f}, {  3784.0f,   -959.0f,  -1241.0f, 0.0f},
    {  3892.0f,      0.0f,  -1276.0f, 0.0f}, {  3784.0f,    959.0f,  -1241.0f, 0.0f},
    {  1260.0f,   3877.0f,    398.0f, 0.0f}, {  1730.0f,   3690.0f,   -410.0f, 0.0f},
    {   769.0f,   4002.0f,   -410.0f, 0.0f}, {  2081.0f,   3303.0f,  -1241.0f, 0.0f},
    {  1203.0f,   3702.0f,  -1276.0f, 0.0f}, {   257.0f,   3895.0f,  -1241.0f, 0.0f},
    { -3298.0f,   2396.0f,    398.0f, 0.0f}, { -2975.0f,   2786.0f,   -410.0f, 0.0f},
    { -3569.0f,   1968.0f,   -410.0f, 0.0f}, { -2498.0f,   3000.0f,  -1241.0f, 0.0f},
    { -3149.0f,   2288.0f,  -1276.0f, 0.0f}, { -3625.0f,   1449.0f,  -1241.0f, 0.0f},
    { -3298.0f,  -2396.0f,    398.0f, 0.0f}, { -3569.0f,  -1968.0f,   -410.0f, 0.0f},
    { -2975.0f,  -2786.0f,   -410.0f, 0.0f}, { -3625.0f,  -1449.0f,  -1241.0f, 0.0f},
    { -3149.0f,  -2288.0f,  -1276.0f, 0.0f}, { -2498.0f,  -3000.0f,  -1241.0f, 0.0f},
    {  1260.0f,  -3877.0f,    398.0f, 0.0f}, {   769.0f,  -4002.0f,   -410.0f, 0.0f},
    {  1730.0f,  -3690.0f,   -410.0f, 0.0f}, {   257.0f,  -3895.0f,  -1241.0f, 0.0f},
    {  1203.0f,  -3702.0f,  -1276.0f, 0.0f}, {  2081.0f,  -3303.0f,  -1241.0f, 0.0f},
    {  3298.0f,   2396.0f,   -398.0f, 0.0f}, {  2975.0f,   2786.0f,    410.0f, 0.0f},
    {  3569.0f,   1968.0f,    410.0f, 0.0f}, {  2498.0f,   3000.0f,   1241.0f, 0.0f},
    {  3149.0f,   2288.0f,   1276.0f, 0.0f}, {  3625.0f,   1449.0f,   1241.0f, 0.0f},
    { -1260.0f,   3877.0f,   -398.0f, 0.0f}, { -1730.0f,   3690.0f,    410.0f, 0.0f},
    {  -769.0f,   4002.0f,    410.0f, 0.0f}, { -2081.0f,   3303.0f,   1241.0f, 0.0f},
    { -1203.0f,   3702.0f,   1276.0f, 0.0f}, {  -257.0f,   3895.0f,   1241.0f, 0.0f},
    { -4077.0f,      0.0f,   -398.0f, 0.0f}, { -4044.0f,   -505.0f,    410.0f, 0.0f},
    { -4044.0f,    505.0f,    410.0f, 0.0f}, { -3784.0f,   -959.0f,   1241.0f, 0.0f},
    { -3892.0f,      0.0f,   1276.0f, 0.0f}, { -3784.0f,    959.0f,   1241.0f, 0.0f},
    { -1260.0f,  -3877.0f,   -398.0f, 0.0f}, {  -769.0f,  -4002.0f,    410.0f, 0.0f},
    { -1730.0f,  -3690.0f,    410.0f, 0.0f}, {  -257.0f,  -3895.0f,   1241.0f, 0.0f},
    { -1203.0f,  -3702.0f,   1276.0f, 0.0f}, { -2081.0f,  -3303.0f,   1241.0f, 0.0f},
    {  3298.0f,  -2396.0f,   -398.0f, 0.0f}, {  3569.0f,  -1968.0f,    410.0f, 0.0f},
    {  2975.0f,  -2786.0f,    410.0f, 0.0f}, {  3625.0f,  -1449.0f,   1241.0f, 0.0f},
    {  3149.0f,  -2288.0f,   1276.0f, 0.0f}, {  2498.0f,  -3000.0f,   1241.0f, 0.0f},
    {   453.0f,   1396.0f,  -3824.0f, 0.0f}, {   192.0f,   2225.0f,  -3434.0f, 0.0f},
    {  1152.0f,   1913.0f,  -3434.0f, 0.0f}, {   -46.0f,   2961.0f,  -2830.0f, 0.0f},
    {   891.0f,   2741.0f,  -2911.0f, 0.0f}, {  1778.0f,   2368.0f,  -2830.0f, 0.0f},
    { -1187.0f,    862.0f,  -3824.0f, 0.0f}, { -2057.0f,    870.0f,  -3434.0f, 0.0f},
    { -1463.0f,   1687.0f,  -3434.0f, 0.0f}, { -2830.0f,    871.0f,  -2830.0f, 0.0f},
    { -2331.0f,   1694.0f,  -2911.0f, 0.0f}, { -1703.0f,   2423.0f,  -2830.0f, 0.0f},
    { -1187.0f,   -862.0f,  -3824.0f, 0.0f}, { -1463.0f,  -1687.0f,  -3434.0f, 0.0f},
    { -2057.0f,   -870.0f,  -3434.0f, 0.0f}, { -1703.0f,  -2423.0f,  -2830.0f, 0.0f},
    { -2331.0f,  -1694.0f,  -2911.0f, 0.0f}, { -2830.0f,   -871.0f,  -2830.0f, 0.0f},
    {   453.0f,  -1396.0f,  -3824.0f, 0.0f}, {  1152.0f,  -1913.0f,  -3434.0f, 0.0f},
    {   192.0f,  -2225.0f,  -3434.0f, 0.0f}, {  1778.0f,  -2368.0f,  -2830.0f, 0.0f},
    {   891.0f,  -2741.0f,  -2911.0f, 0.0f}, {   -46.0f,  -2961.0f,  -2830.0f, 0.0f},
    {  1467.0f,      0.0f,  -3824.0f, 0.0f}, {  2175.0f,    505.0f,  -3434.0f, 0.0f},
    {  2175.0f,   -505.0f,  -3434.0f, 0.0f}, {  2802.0f,    959.0f,  -2830.0f, 0.0f},
    {  2882.0f,      0.0f,  -2911.0f, 0.0f}, {  2802.0f,   -959.0f,  -2830.0f, 0.0f},
};

using NormalTable = const float (*)[4];

// Normal table used by the lighting kernel. With the scratchpad enabled it is
// a copy in the scratchpad normals region (psp/psp_scratchpad.h), refilled
// whenever the region tag was lost; otherwise the .rodata table.
//
// Why: each lit vertex does one lv.q at a data-dependent index of this 4 KB
// table. A character's light pass touches most of its 64 cache lines, and the
// skin and vertex-pack passes that follow stream far more than the 16 KB data
// cache, so every character draw used to refill them from RAM. In the
// scratchpad the lookups never miss and no longer compete with the streams.
inline NormalTable AcquireNormals() {
#if defined(RC_PSP_SCRATCHPAD)
    constexpr std::uint32_t kNormalsTag = 0x4E524D31u;  // "NRM1"
    PspScratchpad::Header* header = PspScratchpad::ValidHeader();
    float (*table)[4] = PspScratchpad::At<float[4]>(PspScratchpad::kNormalsOffset);
    if (header->normalsTag != kNormalsTag) {
        RC_PSP_SCRATCHPAD_COUNT(normalsRefills);
        for (unsigned i = 0; i < 256; ++i) {
            table[i][0] = kNormals[i][0];
            table[i][1] = kNormals[i][1];
            table[i][2] = kNormals[i][2];
            table[i][3] = kNormals[i][3];
        }
        header->normalsTag = kNormalsTag;
    }
    // The kernels read the table from asm without a "memory" clobber; keep
    // the refill stores ahead of them.
    __asm__ volatile("" : : : "memory");
    return table;
#else
    return kNormals;
#endif
}

// --- Lighting -------------------------------------------------------------

// Port light state hoisted out of the per-vertex loop. The callback reads the
// same globals for every vertex; nothing in the joint loop can change them.
// Only lights that can contribute are kept: a black light or a zero
// direction adds exactly 0 in the callback.
struct LightState {
    std::int32_t ambient[3];       // channel << 4
    std::uint32_t count;           // contributing lights
    std::int32_t direction[3][3];  // -(direction >> 4): surface -> light
    std::int32_t colour[3][3];     // channel << 4
    std::int32_t maxDirection;     // largest |direction| component
};

template <typename Light>
inline LightState MakeLightState(std::uint32_t ambient, const Light* const lights[3]) {
    LightState state{};
    for (unsigned c = 0; c < 3; ++c) {
        state.ambient[c] = static_cast<std::int32_t>((ambient >> (8u * c)) & 0xFFu) << 4;
    }
    for (unsigned slot = 0; slot < 3; ++slot) {
        const Light* light = lights[slot];
        if (!light || (light->colour & 0xFFFFFFu) == 0u) {
            continue;  // Missing or black: contributes nothing, as in the callback.
        }
        // >>4 bounds the magnitude by 2^27, so the negation cannot overflow.
        const std::int32_t d[3] = {
            -(light->directionX >> 4), -(light->directionY >> 4), -(light->directionZ >> 4),
        };
        if (d[0] == 0 && d[1] == 0 && d[2] == 0) {
            continue;  // dot == 0: no contribution.
        }
        const std::uint32_t l = state.count++;
        for (unsigned c = 0; c < 3; ++c) {
            state.direction[l][c] = d[c];
            state.colour[l][c] = static_cast<std::int32_t>((light->colour >> (8u * c)) & 0xFFu) << 4;
            const std::int32_t magnitude = d[c] < 0 ? -d[c] : d[c];
            state.maxDirection = magnitude > state.maxDirection ? magnitude : state.maxDirection;
        }
    }
    return state;
}

// True when every |n . l| partial sum of a normal rotated by `m` fits in s32.
// |n_i| <= 4096 * rowSum_i (+1 for rounding), |l_j| <= maxDirection.
inline bool NarrowDotSafe(const LightState& state, const Mat4& m) {
    float rowMax = 0.0f;
    for (unsigned row = 0; row < 3; ++row) {
        const float a = m.m[row] < 0.0f ? -m.m[row] : m.m[row];
        const float b = m.m[4 + row] < 0.0f ? -m.m[4 + row] : m.m[4 + row];
        const float c = m.m[8 + row] < 0.0f ? -m.m[8 + row] : m.m[8 + row];
        const float sum = a + b + c;
        if (!(sum <= 1.0e6f)) {
            return false;  // NaN or huge: use the 64-bit path.
        }
        rowMax = sum > rowMax ? sum : rowMax;
    }
    const float normalBound = rowMax * 4096.0f * 1.0001f + 2.0f;
    return 3.0f * normalBound * static_cast<float>(state.maxDirection) < 2.0e9f;
}

inline std::uint32_t ClampLitChannel(std::int32_t value) {
    if (value <= 0) {
        return 0u;
    }
    value = (value + 8) >> 4;
    return value > 255 ? 255u : static_cast<std::uint32_t>(value);
}

// Integer tail of ComputePsxLitColourFromNormalIndex for a rotated, truncated
// normal. Returns r | g << 8 | b << 16. Narrow requires NarrowDotSafe().
template <bool Narrow>
inline std::uint32_t LitColour(const LightState& state, std::int32_t nx, std::int32_t ny,
                               std::int32_t nz) {
    std::int32_t r = state.ambient[0];
    std::int32_t g = state.ambient[1];
    std::int32_t b = state.ambient[2];
    for (std::uint32_t l = 0; l < state.count; ++l) {
        const std::int32_t* d = state.direction[l];
        std::int32_t intensity;
        if (Narrow) {
            const std::int32_t dot = nx * d[0] + ny * d[1] + nz * d[2];
            // dot <= 0 gives a non-positive shift, which clamps to "no light".
            intensity = dot >> 12;
            intensity = intensity < 0 ? 0 : (intensity > 4096 ? 4096 : intensity);
        }
        else {
            const std::int64_t dot = static_cast<std::int64_t>(nx) * d[0]
                + static_cast<std::int64_t>(ny) * d[1]
                + static_cast<std::int64_t>(nz) * d[2];
            if (dot <= 0) {
                continue;
            }
            const std::int64_t shifted = dot >> 12;
            intensity = shifted > 4096 ? 4096 : static_cast<std::int32_t>(shifted);
        }
        // colour << 4 <= 4080 and intensity <= 4096: the product fits in s32.
        r += (state.colour[l][0] * intensity) >> 12;
        g += (state.colour[l][1] * intensity) >> 12;
        b += (state.colour[l][2] * intensity) >> 12;
    }
    return ClampLitChannel(r) | (ClampLitChannel(g) << 8) | (ClampLitChannel(b) << 16);
}

// Same layout as RP_XformVertsLitCBF_CL: scratch[2v] = g << 8 | r, [2v+1] = b.
inline void StoreLit(std::uint16_t* scratch, std::uint32_t vertex, std::uint32_t colour) {
    scratch[vertex * 2u + 0u] = static_cast<std::uint16_t>(colour & 0xFFFFu);
    scratch[vertex * 2u + 1u] = static_cast<std::uint16_t>((colour >> 16) & 0xFFu);
}

// Portable model of LightRange(): the FPU rotation the VFPU kernel mirrors.
inline void RotateNormalReference(const Mat4& m, std::uint8_t index, std::int32_t& nx,
                                  std::int32_t& ny, std::int32_t& nz,
                                  NormalTable normals = kNormals) {
    float x, y, z;
    Mat4TransformDir(m, normals[index][0], normals[index][1], normals[index][2], x, y, z);
    nx = static_cast<std::int32_t>(x);
    ny = static_cast<std::int32_t>(y);
    nz = static_cast<std::int32_t>(z);
}

// Lights source vertices [start, end) of an 8-byte-per-vertex PSX list
// (normal index at byte 6) with joint lighting matrix `m`.
inline void LightRangeReference(const LightState& state, const Mat4& m,
                                const std::uint8_t* vertexList, std::uint32_t start,
                                std::uint32_t end, std::uint16_t* scratch,
                                NormalTable normals = kNormals) {
    const bool narrow = NarrowDotSafe(state, m);
    for (std::uint32_t v = start; v < end; ++v) {
        std::int32_t nx, ny, nz;
        RotateNormalReference(m, vertexList[v * 8u + 6u], nx, ny, nz, normals);
        StoreLit(scratch, v, narrow ? LitColour<true>(state, nx, ny, nz)
                                    : LitColour<false>(state, nx, ny, nz));
    }
}

// --- Vertex colour modulation ---------------------------------------------

// Skins whose base colours are all PSX bytes (c / 128, c <= 255) can use the
// short colour chain: floor(r * light + 0.5) == (c * light + 64) >> 7 exactly.
inline bool IsColourByte(float value) {
    const float scaled = value * 128.0f;
    if (!(scaled >= 0.0f && scaled <= 255.0f)) {
        return false;
    }
    return static_cast<float>(static_cast<std::int32_t>(scaled)) == scaled;
}

// Float model of the general VFPU chain (each step one VFPU op, all exact or
// the same IEEE rounding as the scalar mul.s/add.s).
inline float ModulateGeneralReference(float base, std::uint32_t light) {
    float x = base * 128.0f;                                   // vscl.t
    x = x + 0.5f;                                              // vadd.t
    float b = static_cast<float>(static_cast<std::int32_t>(x)); // vf2iz.t, vi2f.t
    b = b < 255.0f ? b : 255.0f;                               // vmin.t
    float p = b * static_cast<float>(light);                   // vmul.t
    p = p + 64.0f;                                             // vadd.t
    p = p * (1.0f / 128.0f);                                   // vscl.t
    float out = static_cast<float>(static_cast<std::int32_t>(p)) * (1.0f / 128.0f); // vf2iz, vi2f /2^7
    return out < (255.0f / 128.0f) ? out : (255.0f / 128.0f);  // vmin.t
}

// Float model of the short chain, valid when IsColourByte(base).
inline float ModulateByteReference(float base, std::uint32_t light) {
    float p = base * static_cast<float>(light);                // vmul.t
    p = p + 0.5f;                                              // vadd.t
    float out = static_cast<float>(static_cast<std::int32_t>(p)) * (1.0f / 128.0f); // vf2iz, vi2f /2^7
    return out < (255.0f / 128.0f) ? out : (255.0f / 128.0f);  // vmin.t
}

// --- PSP kernels ----------------------------------------------------------
#if defined(__psp__) && defined(__mips__)
#define PSP_VFPU_SKIN_KERNELS 1

alignas(16) inline constexpr float kVertexConstants[5][4] = {
    {0.5f, 0.5f, 0.5f, 0.5f},                                 // C200
    {255.0f, 255.0f, 255.0f, 255.0f},                         // C210
    {64.0f, 64.0f, 64.0f, 64.0f},                             // C220
    {255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f}, // C230
    {128.0f, 1.0f / 128.0f, 0.0f, 0.0f},                      // S400, S401
};

inline void BeginVertices() {
    __asm__ volatile(
        "lv.q    C200, 0(%0)\n"
        "lv.q    C210, 16(%0)\n"
        "lv.q    C220, 32(%0)\n"
        "lv.q    C230, 48(%0)\n"
        "lv.q    C400, 64(%0)\n"
        : : "r"(kVertexConstants) : "memory");
}

// `m` must be 16-byte aligned (Align16Allocator or the scratchpad joints region).
inline void LoadJoint(const Mat4* m) {
    __asm__ volatile(
        "lv.q    C000, 0(%0)\n"
        "lv.q    C010, 16(%0)\n"
        "lv.q    C020, 32(%0)\n"
        "lv.q    C030, 48(%0)\n"
        : : "r"(m->m) : "memory");
}

// Position: out[0..2] = joint * (lx, ly, lz, 1); E000 is the transposed view,
// so vhtfm4 dots the matrix rows (column-major Mat4 loaded by columns).
// Colour: out[3..5] = min(floor((b * light + 64) / 128), 255) / 128 with
// b = min(trunc(c * 128 + 0.5), 255), identical to the integer scalar path.
inline void SkinLitVertex(const void* sv, float* out, std::uint32_t lightR,
                          std::uint32_t lightG, std::uint32_t lightB) {
    __asm__ volatile(
        "lv.s    S130, 12(%[sv])\n"
        "lv.s    S131, 16(%[sv])\n"
        "lv.s    S132, 20(%[sv])\n"
        "lv.s    S100, 0(%[sv])\n"
        "lv.s    S101, 4(%[sv])\n"
        "lv.s    S102, 8(%[sv])\n"
        "vscl.t  C130, C130, S400\n"
        "mtv     %[lr], S120\n"
        "mtv     %[lg], S121\n"
        "mtv     %[lb], S122\n"
        "vhtfm4.q C110, E000, C100\n"
        "vadd.t  C130, C130, C200\n"
        "vi2f.t  C120, C120, 0\n"
        "lv.s    S700, 24(%[sv])\n"
        "lv.s    S701, 28(%[sv])\n"
        "vf2iz.t C130, C130, 0\n"
        "lv.s    S702, 32(%[sv])\n"
        "lv.s    S703, 36(%[sv])\n"
        "vi2f.t  C130, C130, 0\n"
        "sv.s    S110, 0(%[out])\n"
        "sv.s    S111, 4(%[out])\n"
        "vmin.t  C130, C130, C210\n"
        "sv.s    S112, 8(%[out])\n"
        "sv.s    S700, 24(%[out])\n"
        "vmul.t  C130, C130, C120\n"
        "sv.s    S701, 28(%[out])\n"
        "sv.s    S702, 32(%[out])\n"
        "vadd.t  C130, C130, C220\n"
        "sv.s    S703, 36(%[out])\n"
        "vscl.t  C130, C130, S401\n"
        "vf2iz.t C130, C130, 0\n"
        "vi2f.t  C130, C130, 7\n"
        "vmin.t  C130, C130, C230\n"
        "sv.s    S130, 12(%[out])\n"
        "sv.s    S131, 16(%[out])\n"
        "sv.s    S132, 20(%[out])\n"
        :
        : [sv] "r"(sv), [out] "r"(out), [lr] "r"(lightR), [lg] "r"(lightG), [lb] "r"(lightB)
        : "memory");
}

// Short chain for skins whose base colours are all PSX bytes (IsColourByte):
// floor(c / 128 * light + 0.5) == (c * light + 64) >> 7, then clamp and / 128.
inline void SkinLitByteVertex(const void* sv, float* out, std::uint32_t lightR,
                              std::uint32_t lightG, std::uint32_t lightB) {
    __asm__ volatile(
        "mtv     %[lr], S120\n"
        "mtv     %[lg], S121\n"
        "mtv     %[lb], S122\n"
        "lv.s    S100, 0(%[sv])\n"
        "lv.s    S101, 4(%[sv])\n"
        "lv.s    S102, 8(%[sv])\n"
        "vi2f.t  C120, C120, 0\n"
        "lv.s    S130, 12(%[sv])\n"
        "lv.s    S131, 16(%[sv])\n"
        "lv.s    S132, 20(%[sv])\n"
        "vhtfm4.q C110, E000, C100\n"
        "vmul.t  C130, C130, C120\n"
        "lv.s    S700, 24(%[sv])\n"
        "lv.s    S701, 28(%[sv])\n"
        "vadd.t  C130, C130, C200\n"
        "lv.s    S702, 32(%[sv])\n"
        "lv.s    S703, 36(%[sv])\n"
        "vf2iz.t C130, C130, 0\n"
        "sv.s    S700, 24(%[out])\n"
        "sv.s    S701, 28(%[out])\n"
        "vi2f.t  C130, C130, 7\n"
        "sv.s    S702, 32(%[out])\n"
        "sv.s    S703, 36(%[out])\n"
        "vmin.t  C130, C130, C230\n"
        "sv.s    S110, 0(%[out])\n"
        "sv.s    S111, 4(%[out])\n"
        "sv.s    S112, 8(%[out])\n"
        "sv.s    S130, 12(%[out])\n"
        "sv.s    S131, 16(%[out])\n"
        "sv.s    S132, 20(%[out])\n"
        :
        : [sv] "r"(sv), [out] "r"(out), [lr] "r"(lightR), [lg] "r"(lightG), [lb] "r"(lightB)
        : "memory");
}

// Position only, for the fused packed upload: out[0..2] = joint *
// (lx, ly, lz, 1), the same vhtfm4.q as the full kernels (identical floats).
// `out` needs 4-byte alignment only.
inline void SkinPosition(const void* sv, float* out) {
    __asm__ volatile(
        "lv.s    S100, 0(%[sv])\n"
        "lv.s    S101, 4(%[sv])\n"
        "lv.s    S102, 8(%[sv])\n"
        "vhtfm4.q C110, E000, C100\n"
        "sv.s    S110, 0(%[out])\n"
        "sv.s    S111, 4(%[out])\n"
        "sv.s    S112, 8(%[out])\n"
        :
        : [sv] "r"(sv), [out] "r"(out)
        : "memory");
}

// Unlit vertex: transformed position, base colour and attributes copied.
inline void SkinUnlitVertex(const void* sv, float* out) {
    __asm__ volatile(
        "lv.s    S100, 0(%[sv])\n"
        "lv.s    S101, 4(%[sv])\n"
        "lv.s    S102, 8(%[sv])\n"
        "lv.s    S130, 12(%[sv])\n"
        "vhtfm4.q C110, E000, C100\n"
        "lv.s    S131, 16(%[sv])\n"
        "lv.s    S132, 20(%[sv])\n"
        "lv.s    S700, 24(%[sv])\n"
        "lv.s    S701, 28(%[sv])\n"
        "lv.s    S702, 32(%[sv])\n"
        "lv.s    S703, 36(%[sv])\n"
        "sv.s    S130, 12(%[out])\n"
        "sv.s    S131, 16(%[out])\n"
        "sv.s    S132, 20(%[out])\n"
        "sv.s    S700, 24(%[out])\n"
        "sv.s    S701, 28(%[out])\n"
        "sv.s    S702, 32(%[out])\n"
        "sv.s    S703, 36(%[out])\n"
        "sv.s    S110, 0(%[out])\n"
        "sv.s    S111, 4(%[out])\n"
        "sv.s    S112, 8(%[out])\n"
        :
        : [sv] "r"(sv), [out] "r"(out)
        : "memory");
}

// Columns 0..2 of the joint lighting matrix (any 4-byte alignment).
inline void LoadLightingMatrix(const Mat4& m) {
    __asm__ volatile(
        "lv.s    S500, 0(%0)\n"
        "lv.s    S501, 4(%0)\n"
        "lv.s    S502, 8(%0)\n"
        "lv.s    S510, 16(%0)\n"
        "lv.s    S511, 20(%0)\n"
        "lv.s    S512, 24(%0)\n"
        "lv.s    S520, 32(%0)\n"
        "lv.s    S521, 36(%0)\n"
        "lv.s    S522, 40(%0)\n"
        : : "r"(m.m) : "memory");
}

// Starts C610 = trunc((col0 * nx + col1 * ny) + col2 * nz): the operation
// order and single roundings of Mat4TransformDir, so the truncation matches.
// Reads only the immutable normal table, so no "memory" clobber: the light
// state can stay in registers across the pipelined loop. `normals` is the
// table from AcquireNormals() (scratchpad or .rodata).
inline void IssueNormal(NormalTable normals, std::uint8_t index) {
    __asm__ volatile(
        "lv.q    C600, 0(%0)\n"
        "vscl.t  C610, C500, S600\n"
        "vscl.t  C620, C510, S601\n"
        "vadd.t  C610, C610, C620\n"
        "vscl.t  C620, C520, S602\n"
        "vadd.t  C610, C610, C620\n"
        "vf2iz.t C610, C610, 0\n"
        : : "r"(normals[index]));
}

inline void ReadNormal(std::int32_t& nx, std::int32_t& ny, std::int32_t& nz) {
    __asm__ volatile(
        "mfv     %0, S610\n"
        "mfv     %1, S611\n"
        "mfv     %2, S612\n"
        : "=r"(nx), "=r"(ny), "=r"(nz));
}

// Software-pipelined: the next normal rotates on the VFPU while the integer
// unit evaluates the lights for the current one.
template <bool Narrow>
inline void LightRangeKernel(const LightState& state, NormalTable normals,
                             const std::uint8_t* vertexList, std::uint32_t start,
                             std::uint32_t end, std::uint16_t* scratch) {
    IssueNormal(normals, vertexList[start * 8u + 6u]);
    for (std::uint32_t v = start; v < end; ++v) {
        std::int32_t nx, ny, nz;
        ReadNormal(nx, ny, nz);
        if (v + 1u < end) {
            IssueNormal(normals, vertexList[(v + 1u) * 8u + 6u]);
        }
        StoreLit(scratch, v, LitColour<Narrow>(state, nx, ny, nz));
    }
}

inline void LightRange(const LightState& state, NormalTable normals, const Mat4& m,
                       const std::uint8_t* vertexList, std::uint32_t start,
                       std::uint32_t end, std::uint16_t* scratch) {
    if (start >= end) {
        return;
    }
    LoadLightingMatrix(m);
    if (NarrowDotSafe(state, m)) {
        LightRangeKernel<true>(state, normals, vertexList, start, end, scratch);
    }
    else {
        LightRangeKernel<false>(state, normals, vertexList, start, end, scratch);
    }
}

#endif // __psp__ && __mips__

} // namespace PspVfpuSkin
