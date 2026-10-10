#include "gen/geometry.h"
#include "gen/lights.h"
#include "p3d/matrix.h"
#include "psp/psp_normal_light_cache.h"
#include "psp/psp_skin_transform_cache.h"
#include "psp/psp_packet_lookup.h"
#include "psp/psp_perf.h"
#include "psp/psp_skin_view_cull.h"
#include "psp/psp_vfpu_skin.h"
#include "p3d/skeleton.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static HardwareLight lights[3];
HardwareLight::HardwareLight() = default;
HardwareLight::~HardwareLight() = default;
static u32 ambient;
u32 GetCurrentPortAmbientLightColour() { return ambient; }
const HardwareLight* GetCurrentPortHardwareLight(s32 i) { return &lights[i]; }
// tPrimGeom's destructor releases its (here never built) PSP cache.
namespace PspPerf { void AddRendererMemory(s32) {} }

static void require(bool ok) { if (!ok) std::abort(); }

namespace {

struct CullTestVertex {
    f32 lx, ly, lz;
    u32 jointIdx;
};

std::uint32_t g_cullSeed = 0x2468ace1u;
f32 CullRandom(f32 lo, f32 hi) {
    g_cullSeed = g_cullSeed * 1664525u + 1013904223u;
    return lo + (hi - lo) * static_cast<f32>(g_cullSeed >> 8) / 16777216.0f;
}

Mat4 CullRandomTransform(f32 translation, bool nonUniform) {
    Mat4 rx, ry, rz;
    const f32 ax = CullRandom(-3.2f, 3.2f), ay = CullRandom(-3.2f, 3.2f), az = CullRandom(-3.2f, 3.2f);
    rx.m[5] = std::cos(ax); rx.m[6] = std::sin(ax); rx.m[9] = -std::sin(ax); rx.m[10] = std::cos(ax);
    ry.m[0] = std::cos(ay); ry.m[2] = -std::sin(ay); ry.m[8] = std::sin(ay); ry.m[10] = std::cos(ay);
    rz.m[0] = std::cos(az); rz.m[1] = std::sin(az); rz.m[4] = -std::sin(az); rz.m[5] = std::cos(az);
    Mat4 scale;
    const f32 uniform = CullRandom(0.5f, 2.0f);
    scale.m[0] = nonUniform ? CullRandom(0.3f, 2.5f) : uniform;
    scale.m[5] = nonUniform ? CullRandom(0.3f, 2.5f) : uniform;
    scale.m[10] = nonUniform ? CullRandom(0.3f, 2.5f) : uniform;
    Mat4 result = rz * ry * rx * scale;
    result.m[12] = CullRandom(-translation, translation);
    result.m[13] = CullRandom(-translation, translation);
    result.m[14] = CullRandom(-translation, translation);
    return result;
}

// Every rejected character must have no vertex inside |x|,|y| <= w, across
// random cameras, model/joint rotations, non-uniform scale and both sides of
// the eye; a joint at the look-at target must never be rejected.
void TestSkinViewCull() {
    constexpr unsigned kJoints = 12, kVerts = 96;
    unsigned culled = 0, kept = 0;
    for (unsigned trial = 0; trial < 20000; ++trial) {
        const f32 ex = CullRandom(-4000.0f, 4000.0f), ey = CullRandom(-4000.0f, 4000.0f),
                  ez = CullRandom(-4000.0f, 4000.0f);
        const f32 cx = CullRandom(-4000.0f, 4000.0f), cy = CullRandom(-4000.0f, 4000.0f),
                  cz = CullRandom(-4000.0f, 4000.0f);
        const Mat4 view = LookAt(ex, ey, ez, cx, cy, cz, 0.0f, 1.0f, 0.0f);
        const Mat4 projection = PerspectiveReversedZ(CullRandom(0.6f, 1.6f),
                                                     480.0f / 272.0f, 64.0f, 60000.0f);
        const Mat4 world = CullRandomTransform(4000.0f, trial % 3u == 0u);
        Mat4 joints[kJoints];
        for (Mat4& joint : joints) {
            joint = CullRandomTransform(400.0f, trial % 5u == 0u);
        }
        CullTestVertex verts[kVerts];
        for (unsigned i = 0; i < kVerts; ++i) {
            // Joint 11 owns no vertex and must be ignored.
            verts[i] = { CullRandom(-150.0f, 150.0f), CullRandom(-150.0f, 150.0f),
                         CullRandom(-150.0f, 150.0f), static_cast<u32>(i % (kJoints - 1u)) };
        }
        const std::uint32_t slots = PspSkinViewCull::JointSlotCount(verts, kVerts);
        require(slots == kJoints - 1u);
        f32 radii[kJoints];
        require(PspSkinViewCull::FillJointRadii(verts, kVerts, radii, slots));
        const Mat4 clip = projection * view * world;
        const bool outside = PspSkinViewCull::OutsideView(clip, radii, slots, joints, kJoints);
        bool anyVisible = false;
        for (const CullTestVertex& v : verts) {
            f32 jx, jy, jz;
            Mat4TransformPoint(joints[v.jointIdx], v.lx, v.ly, v.lz, jx, jy, jz);
            const f32 x = clip.m[0] * jx + clip.m[4] * jy + clip.m[8] * jz + clip.m[12];
            const f32 y = clip.m[1] * jx + clip.m[5] * jy + clip.m[9] * jz + clip.m[13];
            const f32 w = clip.m[3] * jx + clip.m[7] * jy + clip.m[11] * jz + clip.m[15];
            anyVisible |= (x >= -w && x <= w && y >= -w && y <= w);
        }
        require(!(outside && anyVisible));
        culled += outside;
        kept += !outside;
    }
    require(culled > 2000 && kept > 2000);

    // A character centred on the look-at point is always kept.
    const Mat4 view = LookAt(0.0f, 500.0f, -3000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    const Mat4 clip = PerspectiveReversedZ(1.0f, 480.0f / 272.0f, 64.0f, 60000.0f) * view;
    Mat4 joint;
    const f32 radius = 100.0f;
    require(!PspSkinViewCull::OutsideView(clip, &radius, 1, &joint, 1));
    // Non-finite input never culls; a joint index outside the skeleton disables it.
    const f32 nanRadius = std::nanf("");
    require(!PspSkinViewCull::OutsideView(clip, &nanRadius, 1, &joint, 1));
    require(!PspSkinViewCull::OutsideView(clip, &radius, 2, &joint, 1));
    const CullTestVertex bad = { 0.0f, 0.0f, 0.0f, 300u };
    require(PspSkinViewCull::JointSlotCount(&bad, 1) == 0);
    std::printf("PASS: skin view rejection conservative, culled=%u kept=%u\n", culled, kept);
}

// Production integer modulation of DrawableSTree::Display (scalar loop).
f32 ProductionModulate(f32 base, u32 light) {
    u32 b = static_cast<u32>(base * 128.0f + 0.5f);
    if (b > 255u) b = 255u;
    u32 mod = (b * light + 64u) >> 7;
    if (mod > 255u) mod = 255u;
    return static_cast<f32>(mod) / 128.0f;
}

bool SameBits(f32 a, f32 b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

// The VFPU path (psp/psp_vfpu_skin.h) must reproduce the production lighting
// callback and colour modulation exactly. The kernels themselves need PSP
// hardware (checked there by the first-draw self-test); here the duplicated
// normal table and every arithmetic step they implement are checked against
// RP_XformVertsLitCBF_CL and the scalar loop.
void TestVfpuSkinReference() {
    // Lighting: all 256 normals through the real callback, random lights and
    // joint matrices (rotation, uniform/non-uniform scale, mirrored), plus
    // extreme directions/scales that force the 64-bit dot path.
    std::vector<u8> vertexList(256u * 8u, 0u);
    for (u32 v = 0; v < 256u; ++v) {
        vertexList[v * 8u + 6u] = static_cast<u8>(v);
        vertexList[v * 8u + 7u] = static_cast<u8>(v * 37u);  // neighbouring bytes are ignored
    }
    tPrimGeom geom;
    geom.SetVertexList(vertexList.data());
    geom.numVerts = 256;
    STreeJoint joint{};
    std::vector<u16> expected(512u), actual(512u);
    u32 narrowStates = 0, wideStates = 0, lightCountSeen[4] = {};
    for (u32 trial = 0; trial < 3000u; ++trial) {
        ambient = static_cast<u32>(CullRandom(0.0f, 16777215.0f)) & 0xFFFFFFu;
        const bool extreme = trial % 7u == 3u;
        for (u32 i = 0; i < 3; ++i) {
            const f32 range = extreme ? 1.0e9f : 65536.0f;
            lights[i].directionX = static_cast<s32>(CullRandom(-range, range));
            lights[i].directionY = static_cast<s32>(CullRandom(-range, range));
            lights[i].directionZ = static_cast<s32>(CullRandom(-range, range));
            lights[i].colour = static_cast<u32>(CullRandom(0.0f, 16777215.0f));
            if (CullRandom(0.0f, 1.0f) < 0.3f) lights[i].colour = 0;     // black light
            if ((trial + i) % 11u == 0u) lights[i].directionX = lights[i].directionY
                = lights[i].directionZ = 0;                               // no direction
            if ((trial + i) % 13u == 0u) lights[i].colour |= 0xFF000000u; // ignored byte
        }
        Mat4 matrix = trial % 9u == 0u ? Mat4() : CullRandomTransform(0.0f, trial % 3u == 0u);
        if (trial % 17u == 0u) matrix.ScaleRotation(-1.0f);
        if (trial % 23u == 0u) matrix.ScaleRotation(60.0f);
        const HardwareLight* const port[3] = { &lights[0], &lights[1], &lights[2] };
        const PspVfpuSkin::LightState state = PspVfpuSkin::MakeLightState(ambient, port);
        lightCountSeen[state.count]++;
        const bool narrow = PspVfpuSkin::NarrowDotSafe(state, matrix);
        narrowStates += narrow;
        wideStates += !narrow;

        const u32 start = trial % 4u, end = 256u - (trial % 3u);
        joint.primGeomStartIdx = static_cast<u16>(start);
        joint.primGeomCount = static_cast<u16>(end - start);
        std::fill(expected.begin(), expected.end(), 0xBEEFu);
        std::fill(actual.begin(), actual.end(), 0xBEEFu);
        RP_XformVertsLitCBF_CL(&geom, &joint, reinterpret_cast<u32*>(&matrix), expected.data());
        PspVfpuSkin::LightRangeReference(state, matrix, vertexList.data(), start, end, actual.data());
        require(expected == actual);
        // The 32-bit dot path must agree with the 64-bit one whenever chosen.
        if (narrow) {
            for (u32 v = 0; v < 256u; ++v) {
                s32 nx, ny, nz;
                PspVfpuSkin::RotateNormalReference(matrix, static_cast<u8>(v), nx, ny, nz);
                require(PspVfpuSkin::LitColour<true>(state, nx, ny, nz)
                        == PspVfpuSkin::LitColour<false>(state, nx, ny, nz));
            }
        }
    }
    require(narrowStates > 1000u && wideStates > 100u);
    require(lightCountSeen[0] && lightCountSeen[1] && lightCountSeen[2] && lightCountSeen[3]);

    // Colour modulation: every PSX byte colour and light byte through both
    // VFPU chains; arbitrary non-byte colours through the general chain.
    u32 checked = 0;
    for (u32 c = 0; c < 256u; ++c) {
        const f32 base = static_cast<f32>(c) / 128.0f;
        require(PspVfpuSkin::IsColourByte(base));
        for (u32 light = 0; light < 256u; ++light) {
            const f32 want = ProductionModulate(base, light);
            require(SameBits(want, PspVfpuSkin::ModulateGeneralReference(base, light)));
            require(SameBits(want, PspVfpuSkin::ModulateByteReference(base, light)));
            ++checked;
        }
    }
    std::vector<f32> others = { 0.7f, 0.5f, 2.0f, 2.5f, 1.0e-9f, 1000.0f,
                                std::nextafter(0.5f / 128.0f, 0.0f), 100.4999f / 128.0f,
                                std::nextafter(255.5f / 128.0f, 0.0f), 255.5f / 128.0f };
    for (u32 i = 0; i < 20000u; ++i) {
        others.push_back(CullRandom(0.0f, 2.2f));
    }
    for (const f32 base : others) {
        for (u32 light = 0; light < 256u; light += (base > 2.0f ? 1u : 17u)) {
            require(SameBits(ProductionModulate(base, light),
                             PspVfpuSkin::ModulateGeneralReference(base, light)));
            ++checked;
        }
    }
    require(!PspVfpuSkin::IsColourByte(0.7f) && !PspVfpuSkin::IsColourByte(2.0f)
            && !PspVfpuSkin::IsColourByte(-1.0f / 128.0f) && !PspVfpuSkin::IsColourByte(std::nanf("")));

    // Joint matrices must be lv.q-aligned across retained resizes.
    std::vector<Mat4, PspVfpuSkin::Align16Allocator<Mat4>> joints;
    for (u32 n = 1; n < 80u; n += 7u) {
        joints.resize(n);
        require((reinterpret_cast<std::uintptr_t>(joints.data()) & 15u) == 0u);
    }
    std::printf("PASS: VFPU skin reference == production lighting/modulation, "
                "3000 light states (narrow=%u wide=%u), %u colour cases\n",
                narrowStates, wideStates, checked);
}

} // namespace

int main() {
    TestSkinViewCull();
    TestVfpuSkinReference();
    std::array<std::uint32_t, 1024> bases;
    std::array<std::uint8_t, 1024> sizes;
    for (unsigned i = 0; i < bases.size(); ++i) {
        bases[i] = i * 60u + 7u;
        sizes[i] = static_cast<std::uint8_t>((i * 17u) % 53u);
    }
    require(PspPacketLookup::IsOrdered(bases.data(), sizes.data(), bases.size()));
    for (std::uint32_t offset = 0; offset < 62000; ++offset) {
        std::size_t expected = bases.size();
        for (std::size_t i = 0; i < bases.size(); ++i) {
            if (sizes[i] && offset >= bases[i] && offset < bases[i] + sizes[i]) {
                expected = i;
                break;
            }
        }
        require(PspPacketLookup::Find(bases.data(), sizes.data(), bases.size(), offset) == expected);
    }
    require(PspPacketLookup::Find(bases.data(), sizes.data(), 0, 0u) == 0);
    const auto savedBase = bases[1];
    bases[1] = bases[0];
    require(!PspPacketLookup::IsOrdered(bases.data(), sizes.data(), bases.size()));
    bases[1] = savedBase;
    sizes[0] = 255;
    require(!PspPacketLookup::IsOrdered(bases.data(), sizes.data(), bases.size()));
    sizes[0] = 0;
    bases.back() = 0xfffffff0u;
    sizes.back() = 32;
    require(!PspPacketLookup::IsOrdered(bases.data(), sizes.data(), bases.size()));
    std::puts("PASS: exact packet lookup, 62000 offsets, gaps/empty packets, invalid range fallback guard");
    PspSkinTransformCache transforms;
    for (unsigned frame = 0; frame < 32; ++frame) {
        transforms.Begin();
        Mat4 matrix;
        matrix.m[0] = -0.75f;
        matrix.m[5] = 0.25f;
        matrix.m[12] = static_cast<float>(frame * 17);
        unsigned hits = 0;
        for (unsigned i = 0; i < 4096; ++i) {
            const auto source = static_cast<std::uint16_t>((i / 4u) % 513u);
            const float x = static_cast<float>(source) - 50.25f;
            const float y = (i & 8) ? -0.0f : 0.0f;
            const float z = (i % 13 == 0) ? 3.0f : -1.0f;
            float expected[3], actual[3];
            Mat4TransformPoint(matrix, x, y, z, expected[0], expected[1], expected[2]);
            hits += transforms.Transform(source, i % 17 == 0 ? 2u : 1u,
                matrix, x, y, z, actual[0], actual[1], actual[2]);
            require(std::memcmp(expected, actual, sizeof(expected)) == 0);
        }
        require(hits > 1000);
    }
    std::puts("PASS: exact skin transforms, collisions, changed inputs/joints, 32 frame resets");
    for (unsigned state = 0; state < 32; ++state) {
        ambient = (state * 0x193717u) & 0xffffffu;
        for (unsigned i = 0; i < 3; ++i) {
            lights[i].colour = (state * 0x371713u + i * 0x711937u) & 0xffffffu;
            lights[i].directionX = static_cast<s32>(state * 3001 + i * 5003) % 131072 - 65536;
            lights[i].directionY = -lights[i].directionX;
            lights[i].directionZ = 65536 - static_cast<s32>(state * 4096);
        }
        Mat4 matrix;
        matrix.m[0] = state & 1 ? -0.75f : 0.5f;
        matrix.m[4] = 0.25f;
        matrix.m[9] = -0.5f;
        PspNormalLightCache cache;
        unsigned evaluations = 0, hits = 0;
        for (unsigned i = 0; i < 4096; ++i) {
            const auto normal = static_cast<u8>(i * 73u);
            auto evaluate = [&](u8 n) {
                u8 r, g, b;
                ComputePsxLitColourFromNormalIndex(n, &matrix, &r, &g, &b);
                return std::uint32_t(r) | (std::uint32_t(g) << 8) | (std::uint32_t(b) << 16);
            };
            const auto expected = evaluate(normal);
            bool hit;
            const auto actual = cache.Get(normal, [&](u8 n) { ++evaluations; return evaluate(n); }, hit);
            hits += hit;
            require(actual == expected);
        }
        require(evaluations == 256 && hits == 3840);
    }
    std::puts("PASS: exact production lighting, 256 normals, 32 light/matrix states, no cross-state reuse");
}
