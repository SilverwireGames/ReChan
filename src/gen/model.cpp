#include "gen/model.h"
#include "psp/psp_perf.h"
#if defined(RC_PLATFORM_PSP)
#include "psp/psp_packet_lookup.h"
#include "psp/psp_scratchpad.h"
#include "psp/psp_skin_view_cull.h"
#include "psp/psp_vfpu_skin.h"
#include "pddi/psp/psprender.h"
// VFPU skinning/lighting (psp/psp_vfpu_skin.h). The transform-cache
// experiment owns the scalar loop, so it keeps the scalar path.
#if defined(PSP_VFPU_SKIN_KERNELS) && !defined(PSP_DISABLE_VFPU_SKIN) \
    && !defined(PSP_SKIN_TRANSFORM_CACHE)
#define RC_PSP_VFPU_SKIN 1
#endif
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_SKIN_TRANSFORM_CACHE)
#include "psp/psp_skin_transform_cache.h"
#endif
#include <cstdio>
#include <set>
#include "gen/animmgr.h"
#include "gen/animmat.h"
#include "gen/animstruct.h"
#include "gen/charmgr.h"
#include "gen/camera.h"
#include "gen/config.h"
#include "gen/display.h"
#include "gen/envmgr.h"
#include "gen/geometry.h"
#include "gen/lights.h"
#include "gen/paramanim.h"
#include "gen/psxmath_helpers.h"
#include "ai/player.h"
#include "gen/skeleton.h"
#include "p3d/byteread.h"
#include "p3d/context.h"
#include "p3d/p3dmath.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include "snd/hmndsnd.h"
#include "pc/log.h"
#if REAL_TEXTURE_RENDERING
#include "extra/realtexture.h"
#endif
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <new>
#include <type_traits>
#include <vector>

#include "extra/shadowcsm.h"

namespace {

template <typename T, typename Alloc>
void ResizeRetainedScratch(std::vector<T, Alloc>& buffer, std::size_t count) {
    if (buffer.capacity() < count) {
        buffer.reserve(count);
    }
    buffer.resize(count);
}

} // namespace

#if defined(RC_PLATFORM_PSP)
namespace {

// The PSP renderer is single-threaded. Keep the largest working set reached
// by the current scene instead of allocating and freeing skinning buffers on
// every draw. This is intentionally split by use so nested mesh draws cannot
// invalidate another scratch buffer.
struct PspModelFrameScratch {
#if defined(PSP_SKIN_TRANSFORM_CACHE)
    PspSkinTransformCache transforms;
#endif
    // 16-byte aligned so the VFPU kernels can lv.q a joint matrix directly.
    std::vector<Mat4, PspVfpuSkin::Align16Allocator<Mat4>> jointMatrices;
    std::vector<Mat4> attackJointMatrices;
    std::vector<Mat4> partJointMatrices;
    std::vector<u16> litScratch;
    std::vector<u8> litWritten;
    std::vector<f32> skinVertexData;
    std::vector<GeoRenderVertex> litGeoVertices;
    std::vector<u32> litGeoColours;
};

PspModelFrameScratch g_pspModelFrameScratch;

// The scratchpad joints region is raw storage written by plain assignment.
static_assert(sizeof(Mat4) == 64 && std::is_trivially_copyable<Mat4>::value
                  && std::is_trivially_destructible<Mat4>::value,
              "scratchpad joint slots hold 64-byte trivially copyable matrices");

// Whole-character view rejection (see psp/psp_skin_view_cull.h). The joint
// radii are derived once from the immutable load-time skin vertices; a
// failed build (bad joint index, non-finite data, no memory) never culls.
bool PspSkinOutsideView(SkinData* skin, const Mat4* jointMatrices, u32 jointCount,
                        const Mat4& modelWorld) {
#if defined(PSP_DISABLE_SKIN_VIEW_CULL)
    (void)skin;
    (void)jointMatrices;
    (void)jointCount;
    (void)modelWorld;
    return false;
#else
    if (!p3d::context || !skin || !skin->verts) {
        return false;
    }
    if (!skin->pspJointRadiusBuilt) {
        skin->pspJointRadiusBuilt = true;
        const u32 slots = PspSkinViewCull::JointSlotCount(skin->verts, skin->numVerts);
        f32* radii = slots ? new (std::nothrow) f32[slots] : nullptr;
        if (radii && PspSkinViewCull::FillJointRadii(skin->verts, skin->numVerts, radii, slots)) {
            skin->pspJointRadius = radii;
            skin->pspJointRadiusCount = slots;
        }
        else {
            delete[] radii;
        }
    }
    if (!skin->pspJointRadius) {
        return false;
    }
    const Mat4 clip = p3d::context->GetProjectionMatrix()
        * p3d::context->GetViewMatrix() * modelWorld;
    return PspSkinViewCull::OutsideView(clip, skin->pspJointRadius,
                                        skin->pspJointRadiusCount,
                                        jointMatrices, jointCount);
#endif
}

// Inputs of the per-vertex skinning loop; all immutable while it runs.
struct PspSkinLoop {
    const SkinVertex* verts;
    const Mat4* joints;
    const u16* lit;
    const u8* litMask;
    u32 litLimit;
};

// Scalar loop: the fallback and the self-test reference. Same arithmetic as
// the generic loop in DrawableSTree::Display. The PSP build uses
// -fno-strict-aliasing, so every float store there forced the compiler to
// reload vertData/skin/scratch pointers and float constants per lane; hoist
// them into restrict locals instead. `outData` is vertex `begin`'s output.
void PspSkinVerticesScalar(const PspSkinLoop& loop, u32 begin, u32 end, f32* outData) {
    const SkinVertex* __restrict skinVerts = loop.verts;
    const Mat4* __restrict joints = loop.joints;
    f32* __restrict out = outData;
    const u16* __restrict lit = loop.lit;
    const u8* __restrict litMask = loop.litMask;
    const u32 litLimit = loop.litLimit;
    for (u32 i = begin; i < end; i++, out += 10) {
        const SkinVertex& sv = skinVerts[i];
        f32 wx, wy, wz;
#if defined(PSP_SKIN_TRANSFORM_CACHE)
        g_pspModelFrameScratch.transforms.Transform(
            sv.sourceIndex, sv.jointIdx, joints[sv.jointIdx],
            sv.lx, sv.ly, sv.lz, wx, wy, wz);
#else
        Mat4TransformPoint(joints[sv.jointIdx], sv.lx, sv.ly, sv.lz, wx, wy, wz);
#endif
        f32 litR = sv.r;
        f32 litG = sv.g;
        f32 litB = sv.b;
        const u32 sourceIndex = static_cast<u32>(sv.sourceIndex);
        if (sourceIndex < litLimit && litMask[sourceIndex] != 0u) {
            const u16 rg = lit[sourceIndex * 2u + 0u];
            const u16 b = lit[sourceIndex * 2u + 1u];
            const u32 lightR = static_cast<u32>(rg & 0xFFu);
            const u32 lightG = static_cast<u32>((rg >> 8) & 0xFFu);
            const u32 lightB = static_cast<u32>(b & 0xFFu);

            u32 baseR = static_cast<u32>(sv.r * 128.0f + 0.5f);
            u32 baseG = static_cast<u32>(sv.g * 128.0f + 0.5f);
            u32 baseB = static_cast<u32>(sv.b * 128.0f + 0.5f);
            if (baseR > 255u) baseR = 255u;
            if (baseG > 255u) baseG = 255u;
            if (baseB > 255u) baseB = 255u;

            u32 modR = (baseR * lightR + 64u) >> 7;
            u32 modG = (baseG * lightG + 64u) >> 7;
            u32 modB = (baseB * lightB + 64u) >> 7;
            if (modR > 255u) modR = 255u;
            if (modG > 255u) modG = 255u;
            if (modB > 255u) modB = 255u;

            litR = static_cast<f32>(modR) / 128.0f;
            litG = static_cast<f32>(modG) / 128.0f;
            litB = static_cast<f32>(modB) / 128.0f;
        }

        out[0] = wx;
        out[1] = wy;
        out[2] = wz;
        out[3] = litR;
        out[4] = litG;
        out[5] = litB;
        out[6] = sv.u;
        out[7] = sv.v;
        out[8] = sv.tpage;
        out[9] = sv.cba;
    }
}

#if defined(RC_PSP_VFPU_SKIN)
static_assert(sizeof(SkinVertex) == PspVfpuSkin::kVertexStrideBytes
              && offsetof(SkinVertex, lx) == 0 && offsetof(SkinVertex, lz) == 8
              && offsetof(SkinVertex, r) == PspVfpuSkin::kVertexColourOffset
              && offsetof(SkinVertex, b) == PspVfpuSkin::kVertexColourOffset + 8
              && offsetof(SkinVertex, u) == PspVfpuSkin::kVertexAttribOffset
              && offsetof(SkinVertex, cba) == PspVfpuSkin::kVertexAttribOffset + 12,
              "VFPU skin kernels read SkinVertex fields by byte offset");

// Untested until the first lit character draw compares both paths.
enum class PspVfpuSkinMode : u8 { Untested, Enabled, Disabled };
PspVfpuSkinMode g_pspVfpuSkinMode = PspVfpuSkinMode::Untested;
// Fused upload equivalence check (Display): uploads still to compare, and
// whether a mismatch turned the fused path off.
u32 g_pspFusedSkinChecksLeft = 16;
bool g_pspFusedSkinDisabled = false;

} // namespace

// Self-test outcome, RAM only (release builds do not log): read the symbol
// with the PSPLink debugger. magic == 'VFPU' once the test has run.
struct PspVfpuSkinSelfTestRecord {
    u32 magic;
    u32 passed;
    u32 vertices;
    u32 litSources;
    u32 byteColours;
    u32 maxPositionDiffMicro;
    u32 positionFailures;
    u32 colourDiff;   // 1/128 colour units
    u32 lightDiff;    // lit scratch channel units
    u32 attribMismatches;
};
extern "C" {
PspVfpuSkinSelfTestRecord g_pspVfpuSkinSelfTestRecord = {};
}

namespace {

bool PspSkinColoursAreBytes(SkinData* skin) {
    if (!skin->pspColourBytesBuilt) {
        skin->pspColourBytesBuilt = true;
        bool bytes = true;
        for (u32 i = 0; i < skin->numVerts && bytes; i++) {
            const SkinVertex& sv = skin->verts[i];
            bytes = PspVfpuSkin::IsColourByte(sv.r) && PspVfpuSkin::IsColourByte(sv.g)
                && PspVfpuSkin::IsColourByte(sv.b);
        }
        skin->pspColourBytes = bytes;
    }
    return skin->pspColourBytes;
}

// VFPU equivalent of PspSkinVerticesScalar. Positions differ from the FPU
// sum by at most a few ulps; colours and attributes are bit-identical.
template <bool ByteColours>
void PspSkinVerticesVfpu(const PspSkinLoop& loop, u32 begin, u32 end, f32* out) {
    const SkinVertex* sv = loop.verts + begin;
    const Mat4* joints = loop.joints;
    const u16* lit = loop.lit;
    const u8* litMask = loop.litMask;
    const u32 litLimit = loop.litLimit;
    PspVfpuSkin::BeginVertices();
    u32 loadedJoint = 0xFFFFFFFFu;
    for (u32 i = begin; i < end; i++, sv++, out += 10) {
        const u32 joint = sv->jointIdx;
        if (joint != loadedJoint) {
            loadedJoint = joint;
            PspVfpuSkin::LoadJoint(&joints[joint]);
        }
        const u32 sourceIndex = static_cast<u32>(sv->sourceIndex);
        if (sourceIndex < litLimit && litMask[sourceIndex] != 0u) {
            const u32 rg = lit[sourceIndex * 2u + 0u];
            const u32 b = lit[sourceIndex * 2u + 1u];
            if (ByteColours) {
                PspVfpuSkin::SkinLitByteVertex(sv, out, rg & 0xFFu, (rg >> 8) & 0xFFu, b & 0xFFu);
            }
            else {
                PspVfpuSkin::SkinLitVertex(sv, out, rg & 0xFFu, (rg >> 8) & 0xFFu, b & 0xFFu);
            }
        }
        else {
            PspVfpuSkin::SkinUnlitVertex(sv, out);
        }
    }
}

// Fused character upload (byte-colour skins): VFPU position plus integer
// colour and UV written straight into the prim buffer's compact stream. Same
// values as PspSkinVerticesVfpu<true> followed by SetDynamicVertexData's
// compact-only path, without the 40-byte float vertex and the second pass:
// - position: the same vhtfm4.q on the same joint matrix;
// - colour: the scalar reference's integer chain (c = colour * 128 exactly,
//   since PspSkinColoursAreBytes; lit m = min((c * light + 64) >> 7, 255)),
//   then the GU byte MakeGeometryColor gives m / 128;
// - UV: the same float to s16 conversion.
// The input is the skin's compact stream when it has one (conversions done
// once, PspSkinStreamVertex), else its SkinVertex array.
struct PspSkinPackedInput {
    u32 joint, r, g, b, source;
    s16 u, v;
};
inline PspSkinPackedInput ReadPackedInput(const SkinVertex& sv) {
    return {sv.jointIdx, static_cast<u32>(sv.r * 128.0f), static_cast<u32>(sv.g * 128.0f),
            static_cast<u32>(sv.b * 128.0f), static_cast<u32>(sv.sourceIndex),
            static_cast<s16>(sv.u), static_cast<s16>(sv.v)};
}
inline PspSkinPackedInput ReadPackedInput(const PspSkinStreamVertex& sv) {
    return {sv.joint, sv.r, sv.g, sv.b, sv.source, sv.u, sv.v};
}

template <typename Vertex>
void PspSkinPackedVfpu(const PspSkinLoop& loop, const Vertex* sv, u32 count, PspPackedVertex* out) {
    const Mat4* joints = loop.joints;
    const u16* lit = loop.lit;
    const u8* litMask = loop.litMask;
    const u32 litLimit = loop.litLimit;
    const auto guByte = [](u32 value) -> u32 {
        return value >= 128u ? 255u : (value * 255u) >> 7;  // MakeGeometryColor(value / 128)
    };
    const auto litChannel = [](u32 base, u32 light) -> u32 {
        const u32 value = (base * light + 64u) >> 7;
        return value > 255u ? 255u : value;
    };
    u32 loadedJoint = 0xFFFFFFFFu;
    for (u32 i = 0; i < count; i++, sv++, out++) {
        const PspSkinPackedInput in = ReadPackedInput(*sv);
        if (in.joint != loadedJoint) {
            loadedJoint = in.joint;
            PspVfpuSkin::LoadJoint(&joints[in.joint]);
        }
        PspVfpuSkin::SkinPosition(sv, &out->x);
        u32 r = in.r;
        u32 g = in.g;
        u32 b = in.b;
        if (in.source < litLimit && litMask[in.source] != 0u) {
            const u32 rg = lit[in.source * 2u + 0u];
            const u32 lb = lit[in.source * 2u + 1u];
            r = litChannel(r, rg & 0xFFu);
            g = litChannel(g, (rg >> 8) & 0xFFu);
            b = litChannel(b, lb & 0xFFu);
        }
        out->color = guByte(r) | (guByte(g) << 8) | (guByte(b) << 16) | 0xFF000000u;
        out->u = in.u;
        out->v = in.v;
    }
}

// The skin's compact stream, built on first use; null (use `verts`) when a
// joint index does not fit a byte or memory is short. Byte-colour skins only.
const PspSkinStreamVertex* AcquirePspSkinStream(SkinData* skin) {
    if (skin->pspStreamBuilt) {
        return skin->pspStream;
    }
    skin->pspStreamBuilt = true;
    PspSkinStreamVertex* stream = new (std::nothrow) PspSkinStreamVertex[skin->numVerts];
    if (!stream) {
        return nullptr;
    }
    for (u32 i = 0; i < skin->numVerts; i++) {
        const SkinVertex& sv = skin->verts[i];
        if (sv.jointIdx > 0xFFu) {
            delete[] stream;
            return nullptr;
        }
        const PspSkinPackedInput in = ReadPackedInput(sv);
        stream[i] = {sv.lx, sv.ly, sv.lz,
                     static_cast<u8>(in.r), static_cast<u8>(in.g), static_cast<u8>(in.b),
                     static_cast<u8>(in.joint), in.u, in.v, static_cast<u16>(in.source), 0u};
    }
    skin->pspStream = stream;
    return stream;
}

// VFPU equivalent of the RP_XformVertsLitCBF_CL joint loop in Display: same
// joint range clamping, same lighting matrices, bit-identical lit scratch.
// Returns the last joint the callback would have been called for, or -1.
// Kept out of line: inside Display the light state would not fit in registers.
__attribute__((noinline))
s32 PspLightJointsVfpu(const OriginalSTree* renderSource, const STreeData* skel,
                       const Mat4* joints, const Mat4& lightingWorld, u32 sourceVertCount,
                       u16* scratch, u8* written) {
    PspPerf::ScopeTimer lightTimer(PspPerf::RendererCounter::SkinLightUs);
    const HardwareLight* const lights[3] = {
        GetCurrentPortHardwareLight(0),
        GetCurrentPortHardwareLight(1),
        GetCurrentPortHardwareLight(2),
    };
    const PspVfpuSkin::LightState state =
        PspVfpuSkin::MakeLightState(GetCurrentPortAmbientLightColour(), lights);
    const u8* vertexList = renderSource->primGeom->GetVertexList();
    const PspVfpuSkin::NormalTable normals = PspVfpuSkin::AcquireNormals();
    s32 lastJoint = -1;
    for (u32 jointIndex = 0; jointIndex < skel->numJoints; jointIndex++) {
        const STreeJoint& joint = skel->joints[jointIndex];
        if (joint.primGeomCount == 0) {
            continue;
        }
        const u32 startIndex = static_cast<u32>(joint.primGeomStartIdx);
        const u32 endIndex = startIndex + static_cast<u32>(joint.primGeomCount);
        if (startIndex >= sourceVertCount) {
            continue;
        }
        const u32 clampedEnd = (endIndex > sourceVertCount) ? sourceVertCount : endIndex;
        if (clampedEnd <= startIndex) {
            continue;
        }
        std::memset(written + startIndex, 1, clampedEnd - startIndex);
        lastJoint = static_cast<s32>(jointIndex);
        if (!vertexList) {
            continue;  // The callback writes nothing without a vertex list.
        }
        const Mat4 lightingMatrix = lightingWorld * joints[jointIndex];
        PspVfpuSkin::LightRange(state, normals, lightingMatrix, vertexList, startIndex, clampedEnd,
                                scratch);
        PspPerf::CountRenderer(PspPerf::RendererCounter::LightEvaluations, clampedEnd - startIndex);
    }
    return lastJoint;
}

// One-time comparison of the VFPU kernels with the scalar path on a real
// character. Any contract violation keeps the scalar path for the session.
struct PspVfpuSkinSelfTest {
    bool lightCompared = false;
    u32 litVertices = 0;
    u32 lightDiff = 0;       // max |channel| difference of the lit scratch
    u32 colourDiff = 0;      // max output colour difference, in 1/128 units
    u32 attribMismatches = 0;
    u32 positionFailures = 0;
    f32 maxPositionDiff = 0.0f;
    u32 vertices = 0;

    void CompareLight(const u16* expected, const u16* actual, const u8* written, u32 count) {
        lightCompared = true;
        for (u32 v = 0; v < count; v++) {
            if (!written[v]) {
                continue;
            }
            litVertices++;
            const u32 e = expected[v * 2u] | (static_cast<u32>(expected[v * 2u + 1u]) << 16);
            const u32 a = actual[v * 2u] | (static_cast<u32>(actual[v * 2u + 1u]) << 16);
            for (u32 shift = 0; shift < 24; shift += 8) {
                const s32 d = static_cast<s32>((e >> shift) & 0xFFu) - static_cast<s32>((a >> shift) & 0xFFu);
                const u32 magnitude = static_cast<u32>(d < 0 ? -d : d);
                lightDiff = magnitude > lightDiff ? magnitude : lightDiff;
            }
        }
    }

    void CompareVertices(const PspSkinLoop& loop, u32 begin, const f32* expected,
                         const f32* actual, u32 count) {
        for (u32 n = 0; n < count; n++, expected += 10, actual += 10) {
            vertices++;
            const SkinVertex& sv = loop.verts[begin + n];
            const Mat4& m = loop.joints[sv.jointIdx];
            for (u32 axis = 0; axis < 3; axis++) {
                if (!std::isfinite(expected[axis])) {
                    continue;  // Degenerate joint: nothing meaningful to compare.
                }
                // Allow a few ulps of the summed term magnitudes (vhtfm4 dot).
                const f32 scale = std::fabs(m.m[axis] * sv.lx) + std::fabs(m.m[4 + axis] * sv.ly)
                    + std::fabs(m.m[8 + axis] * sv.lz) + std::fabs(m.m[12 + axis]);
                const f32 diff = std::fabs(expected[axis] - actual[axis]);
                if (!(diff <= 1.0e-3f + 4.0e-6f * scale)) {
                    positionFailures++;
                }
                if (diff > maxPositionDiff || diff != diff) {
                    maxPositionDiff = diff;
                }
            }
            for (u32 c = 3; c < 6; c++) {
                const f32 d = std::fabs(expected[c] - actual[c]) * 128.0f;
                const u32 units = (d == d && d < 65536.0f) ? static_cast<u32>(d + 0.5f) : 65535u;
                colourDiff = units > colourDiff ? units : colourDiff;
            }
            if (std::memcmp(expected + 6, actual + 6, 4 * sizeof(f32)) != 0) {
                attribMismatches++;
            }
        }
    }

    // The scalar outputs are the ones drawn this frame either way.
    bool Passed() const {
        return vertices > 0 && (!lightCompared || litVertices > 0) && lightDiff <= 1u
            && colourDiff <= 1u && attribMismatches == 0 && positionFailures == 0;
    }
};

// Runs the VFPU vertex kernels in fixed-size chunks against the scalar output.
void PspVfpuSkinSelfTestVertices(PspVfpuSkinSelfTest& test, const PspSkinLoop& loop,
                                 u32 vertCount, const f32* scalarOut, bool byteColours) {
    static f32 chunk[64 * 10];
    for (u32 begin = 0; begin < vertCount; begin += 64u) {
        const u32 end = (vertCount - begin > 64u) ? begin + 64u : vertCount;
        PspSkinVerticesVfpu<false>(loop, begin, end, chunk);
        test.CompareVertices(loop, begin, scalarOut + begin * 10u, chunk, end - begin);
        if (byteColours) {
            PspSkinVerticesVfpu<true>(loop, begin, end, chunk);
            test.CompareVertices(loop, begin, scalarOut + begin * 10u, chunk, end - begin);
        }
    }
}
#endif // RC_PSP_VFPU_SKIN

} // namespace
#endif

// Draws a mesh buffer, using the real-texture rendering path (one bound real
// 2D texture per material, sub-range draws) when it's enabled and the mesh
// has grouped index ranges; otherwise falls back to the single legacy draw
// that samples the shared PSX VRAM atlas, unchanged from before this feature.
static void DrawTexturedMesh(pddiPrimBuffer* buffer
#if REAL_TEXTURE_RENDERING
                              , const std::vector<RealTextureGroup>* groups
#endif
                              ) {
    if (!buffer) return;

#if REAL_TEXTURE_RENDERING
    // realTextureModeEnabled is the persistent user toggle (set only by the
    // debug UI) -- never flip it here, just gate on it and manage the
    // per-draw texture binding, clearing it afterward so unrelated draws
    // (weffect/trail/shadows/etc, which never call SetTexture themselves)
    // don't pick up a stale real texture left bound from this mesh.
    if (groups && !groups->empty() && p3d::context->IsRealTextureModeEnabled()) {
        for (const RealTextureGroup& group : *groups) {
            const RealTextureEntry* entry = RealTextureRegistry::Instance().Find(group.tpage, group.cba);
            if (entry && entry->texture) {
                p3d::context->SetTexture(entry->texture);
                p3d::context->SetRealTextureRect(entry->offsetX, entry->offsetY, entry->sizeX, entry->sizeY);
            }
            else {
                p3d::context->SetTexture(nullptr);
            }
            p3d::context->DrawPrimBuffer(buffer, group.startIndex, group.indexCount);
        }
        p3d::context->SetTexture(nullptr);
        return;
    }
#endif

    p3d::context->DrawPrimBuffer(buffer);
}

#if MODERN_GRAPHICS
static bool ShouldReceiveModelShadows(const Model* model, const Thing* owner) {
    return model
        && owner
        && !ShadowCSM::IsCasterPrepass()
        && ShadowCSM::GetQuality() != SHADOW_QUALITY_OFF
        && (owner->flags & 0x80u) == 0u
        && (owner->flags & 0x100u) == 0u
        && (model->modelFlags & 0x20u) == 0u;
}

// Per-instance ID for shadow self-exclusion
static u32 GetShadowInstanceId(const Thing* owner) {
    if (!owner) {
        return 0;
    }
    const u32 id = static_cast<u32>(reinterpret_cast<uintptr_t>(owner));
    return id != 0 ? id : 0xFFFFFFFFu;
}
#endif

static const u8 kStreeMirrorSwapPairs[] = {
    2, 6,
    3, 7,
    4, 8,
    5, 9,
    0, 0,
};

static const char* const kStreeMirrorNamePairs[][2] = {
    { "Bip01 L Clavicle", "Bip01 R Clavicle" },
    { "Bip01 L UpperArm", "Bip01 R UpperArm" },
    { "Bip01 L Forearm", "Bip01 R Forearm" },
    { "Bip01 L Hand", "Bip01 R Hand" },
    { "Bip01 L Finger0", "Bip01 R Finger0" },
    { "Bip01 L Finger01", "Bip01 R Finger01" },
    { "Bip01 L Finger02", "Bip01 R Finger02" },
    { "Bip01 L Finger1", "Bip01 R Finger1" },
    { "Bip01 L Finger11", "Bip01 R Finger11" },
    { "Bip01 L Finger12", "Bip01 R Finger12" },
    { "Bip01 L Finger2", "Bip01 R Finger2" },
    { "Bip01 L Finger21", "Bip01 R Finger21" },
    { "Bip01 L Finger22", "Bip01 R Finger22" },
    { "Bip01 L Thigh", "Bip01 R Thigh" },
    { "Bip01 L Calf", "Bip01 R Calf" },
    { "Bip01 L Foot", "Bip01 R Foot" },
    { "Bip01 L Toe0", "Bip01 R Toe0" },
};

static constexpr u32 kDisplayFlagApplyGeoLighting = 0x40000000u;

static std::vector<const OriginalSTree*> s_liveOriginalSTrees;

static void RegisterLiveOriginalSTree(const OriginalSTree* tree) {
    if (!tree) {
        return;
    }

    s_liveOriginalSTrees.push_back(tree);
}

static void UnregisterLiveOriginalSTree(const OriginalSTree* tree) {
    if (!tree) {
        return;
    }

    const auto it = std::find(s_liveOriginalSTrees.begin(), s_liveOriginalSTrees.end(), tree);
    if (it != s_liveOriginalSTrees.end()) {
        s_liveOriginalSTrees.erase(it);
    }
}

static bool FindMappedJointParamByName(const STreeData* skeleton, u32 nameHash, u32& outParam) {
    if (!skeleton || !skeleton->joints || !skeleton->jointOrderMap) {
        return false;
    }

    for (u32 param = 0; param < skeleton->numMapEntries; param++) {
        const u32 jointIndex = skeleton->jointOrderMap[param];
        if (jointIndex < skeleton->numJoints && skeleton->joints[jointIndex].nameUID == nameHash) {
            outParam = param;
            return true;
        }
    }

    return false;
}

static bool SwapMirroredJointParamsByName(const STreeData* skeleton, u32* mirroredMap) {
    if (!skeleton || !mirroredMap) {
        return false;
    }

    bool swappedAny = false;
    for (const auto& pair : kStreeMirrorNamePairs) {
        u32 lhs = 0;
        u32 rhs = 0;
        if (!FindMappedJointParamByName(skeleton, p3dHash(pair[0]), lhs) ||
            !FindMappedJointParamByName(skeleton, p3dHash(pair[1]), rhs)) {
            continue;
        }

        const u32 temp = mirroredMap[lhs];
        mirroredMap[lhs] = mirroredMap[rhs];
        mirroredMap[rhs] = temp;
        swappedAny = true;
    }

    return swappedAny;
}

static bool IsLiveOriginalSTree(const OriginalSTree* tree) {
    if (!tree) {
        return false;
    }

    return std::find(s_liveOriginalSTrees.begin(), s_liveOriginalSTrees.end(), tree) != s_liveOriginalSTrees.end();
}

static void BuildMat4FromPsxPackedMatrix(const s32* packedMatrix, Mat4& out);
static DrawableSTree* GetDrawableSTree(Model* model);

// PSX: SetSemiMode__13OriginalSTreei (MODEL.CPP:3459, 0x8007205C)
static s32 SetSemiMode__13OriginalSTreei(OriginalSTree* original, s32 mode) {
    MARKFUNCTION(0x8007205C);

    if (!original) {
        return 0;
    }

    if (original->skinData) {
        original->skinData->usesSemiTrans = (mode != 0);
    }

    tPrimGeom* primGeom = original->primGeom;
    if (!primGeom
        || !primGeom->ownedRawData
        || !primGeom->ownedRawSize
        || !primGeom->primList
        || !primGeom->loopPrimData
        || primGeom->numLoops == 0) {
        return 0;
    }

    if (primGeom->primList < primGeom->ownedRawData
        || primGeom->loopPrimData < primGeom->ownedRawData) {
        return 0;
    }

    const u32 primStartOffset = static_cast<u32>(primGeom->primList - primGeom->ownedRawData);
    const u32 loopPrimOffset = static_cast<u32>(primGeom->loopPrimData - primGeom->ownedRawData);
    if (primStartOffset >= primGeom->ownedRawSize
        || loopPrimOffset >= primGeom->ownedRawSize
        || (loopPrimOffset + static_cast<u32>(primGeom->numLoops) * 8u) > primGeom->ownedRawSize) {
        return 0;
    }

    u8* packetCursor = primGeom->ownedRawData + primStartOffset;
    u8* packetEnd = primGeom->ownedRawData + primGeom->ownedRawSize;

    for (u32 loopIndex = 0; loopIndex < primGeom->numLoops; loopIndex++) {
        const u8* loopPrim = primGeom->loopPrimData + loopIndex * 8u;
        for (u32 bucket = 0; bucket < 4u; bucket++) {
            const u32 primCount = static_cast<u32>(p3dReadU16LE(loopPrim + bucket * 2u));
            const u32 packetSize = (bucket == 0u) ? 52u : (bucket == 1u) ? 36u : (bucket == 2u) ? 40u : 28u;

            for (u32 i = 0; i < primCount; i++) {
                if (packetCursor + packetSize > packetEnd) {
                    return 0;
                }

                if (mode != 0) {
                    packetCursor[7] = static_cast<u8>(packetCursor[7] | 0x2u);
                }
                else {
                    packetCursor[7] = static_cast<u8>(packetCursor[7] & 0xFDu);
                }

                packetCursor += packetSize;
            }
        }
    }

    return 1;
}

static void BuildMat4FromPsxPackedMatrix(const s32* packedMatrix, Mat4& out) {
    out = Mat4();
    if (!packedMatrix) {
        return;
    }

    static constexpr f32 kInvQ12 = 1.0f / 4096.0f;
    const s16* rot = reinterpret_cast<const s16*>(packedMatrix);
    out.m[0] = (f32)rot[0] * kInvQ12;
    out.m[1] = (f32)rot[1] * kInvQ12;
    out.m[2] = (f32)rot[2] * kInvQ12;
    out.m[4] = (f32)rot[3] * kInvQ12;
    out.m[5] = (f32)rot[4] * kInvQ12;
    out.m[6] = (f32)rot[5] * kInvQ12;
    out.m[8] = (f32)rot[6] * kInvQ12;
    out.m[9] = (f32)rot[7] * kInvQ12;
    out.m[10] = (f32)rot[8] * kInvQ12;
}

// PSX: MakeBillboardMatrix (MODEL.CPP:3199, 0x80071AF4)
void MakeBillboardMatrix(const LVector& pos, Mat4& out, s32 lockY) {
    MARKFUNCTION(0x80071AF4);

    if (!g_display || !g_display->GetCamera()) {
        out = Mat4();
        p3dFillTransMatrix(pos, out);
        return;
    }

    const LVector& cameraPos = g_display->GetCamera()->GetPosition();
    const Vec3 heading(
        FIX16_TO_FLOAT(cameraPos.x - pos.x),
        (lockY == 1) ? 0.0f : FIX16_TO_FLOAT(cameraPos.y - pos.y),
        FIX16_TO_FLOAT(cameraPos.z - pos.z));

    const Vec3 up(0.0f, 1.0f, 0.0f);
    out = Mat4();
    p3dFillHeadingMatrix(heading, up, out);
    p3dFillTransMatrix(pos, out);
}

// PSX: MakeBillboardMatrixFlip (MODEL.CPP:3230, 0x80071BCC)
void MakeBillboardMatrixFlip(const LVector& pos, Mat4& out, s32 lockY) {
    MARKFUNCTION(0x80071BCC);

    if (!g_display || !g_display->GetCamera()) {
        out = Mat4();
        p3dFillTransMatrix(pos, out);
        return;
    }

    const LVector& cameraPos = g_display->GetCamera()->GetPosition();
    const Vec3 heading(
        FIX16_TO_FLOAT(pos.x - cameraPos.x),
        (lockY == 1) ? 0.0f : FIX16_TO_FLOAT(pos.y - cameraPos.y),
        FIX16_TO_FLOAT(pos.z - cameraPos.z));

    const Vec3 up(0.0f, 1.0f, 0.0f);
    out = Mat4();
    p3dFillHeadingMatrix(heading, up, out);
    p3dFillTransMatrix(pos, out);
}

static bool BuildLitPacketScratch(const OriginalSTree* renderSource, std::vector<u8>* outPackets) {
    if (!renderSource || !outPackets || !renderSource->primGeom || !renderSource->fixUpPolysCallback) {
        return false;
    }

    const tPrimGeom* geom = renderSource->primGeom;
    if (!geom->ownedRawData || !geom->primList || !geom->loopPrimData || !geom->ownedRawSize) {
        return false;
    }

    const u32 primStartOffset = static_cast<u32>(geom->primList - geom->ownedRawData);
    if (primStartOffset >= geom->ownedRawSize) {
        return false;
    }

    const u32 primRegionSize = geom->ownedRawSize - primStartOffset;
    outPackets->assign(geom->primList, geom->primList + primRegionSize);
    u8* cursor = outPackets->data();
    u32 polyIndex = 0;
    for (u32 loopIndex = 0; loopIndex < geom->numLoops; loopIndex++) {
        cursor = renderSource->fixUpPolysCallback(renderSource->primGeom, cursor, loopIndex, polyIndex);
        if (!cursor) {
            return false;
        }

        const u8* loopPrim = geom->loopPrimData + loopIndex * 8u;
        polyIndex += static_cast<u32>(p3dReadU16LE(loopPrim + 0));
        polyIndex += static_cast<u32>(p3dReadU16LE(loopPrim + 2));
        polyIndex += static_cast<u32>(p3dReadU16LE(loopPrim + 4));
        polyIndex += static_cast<u32>(p3dReadU16LE(loopPrim + 6));
    }

    return true;
}

static void ApplyLitPacketColoursToSkin(const SkinData* skin,
                                        const std::vector<u8>& litPackets,
                                        std::vector<f32>* vertData) {
    if (!skin || !vertData || litPackets.empty()) {
        return;
    }

    const u8* cursor = litPackets.data();
    const u8* end = litPackets.data() + litPackets.size();
    for (u32 primIndex = 0; primIndex < skin->numPrims; primIndex++) {
        if (cursor + 8 > end) {
            break;
        }

        const u8 cmdBase = static_cast<u8>(cursor[7] & 0xFCu);
        u32 primitiveSize = 0;
        u32 colourOffsets[4] = {};
        u32 colourCount = 0;

        if (cmdBase == 0x3Cu) {
            primitiveSize = 52u;
            colourOffsets[0] = 4u;
            colourOffsets[1] = 16u;
            colourOffsets[2] = 28u;
            colourOffsets[3] = 40u;
            colourCount = 4u;
        }
        else if (cmdBase == 0x2Cu) {
            primitiveSize = 40u;
            colourOffsets[0] = 4u;
            colourOffsets[1] = 4u;
            colourOffsets[2] = 4u;
            colourOffsets[3] = 4u;
            colourCount = 4u;
        }
        else if (cmdBase == 0x38u) {
            primitiveSize = 36u;
            colourOffsets[0] = 4u;
            colourOffsets[1] = 12u;
            colourOffsets[2] = 20u;
            colourOffsets[3] = 28u;
            colourCount = 4u;
        }
        else if (cmdBase == 0x28u) {
            primitiveSize = 36u;
            colourOffsets[0] = 4u;
            colourOffsets[1] = 4u;
            colourOffsets[2] = 4u;
            colourOffsets[3] = 4u;
            colourCount = 4u;
        }
        else if (cmdBase == 0x34u) {
            primitiveSize = 40u;
            colourOffsets[0] = 4u;
            colourOffsets[1] = 16u;
            colourOffsets[2] = 28u;
            colourCount = 3u;
        }
        else if (cmdBase == 0x24u) {
            primitiveSize = 32u;
            colourOffsets[0] = 4u;
            colourOffsets[1] = 4u;
            colourOffsets[2] = 4u;
            colourCount = 3u;
        }
        else if (cmdBase == 0x30u) {
            primitiveSize = 28u;
            colourOffsets[0] = 4u;
            colourOffsets[1] = 12u;
            colourOffsets[2] = 20u;
            colourCount = 3u;
        }
        else if (cmdBase == 0x20u) {
            primitiveSize = 28u;
            colourOffsets[0] = 4u;
            colourOffsets[1] = 4u;
            colourOffsets[2] = 4u;
            colourCount = 3u;
        }
        else {
            break;
        }

        if (cursor + primitiveSize > end) {
            break;
        }

        const u32 base = skin->primStart[primIndex];
        for (u32 colourIndex = 0; colourIndex < colourCount; colourIndex++) {
            const u32 vertexIndex = base + colourIndex;
            if (vertexIndex >= skin->numVerts) {
                continue;
            }

            const u32 offset = colourOffsets[colourIndex];
            (*vertData)[vertexIndex * 10u + 3u] = static_cast<f32>(cursor[offset + 0u]) / 128.0f;
            (*vertData)[vertexIndex * 10u + 4u] = static_cast<f32>(cursor[offset + 1u]) / 128.0f;
            (*vertData)[vertexIndex * 10u + 5u] = static_cast<f32>(cursor[offset + 2u]) / 128.0f;
        }

        cursor += primitiveSize;
    }
}

static DrawableSTree* GetDrawableSTree(Model* model) {
    if (!model || model->drawableType != 2 || !model->drawable) {
        return nullptr;
    }
    return static_cast<DrawableSTree*>(model->drawable);
}

static void SetSemiModeForDrawableSTree(DrawableSTree* drawableStree, s32 mode) {
    if (!drawableStree) {
        return;
    }

    OriginalSTree* originalStree = drawableStree->GetOriginalSTree();
    OriginalSTree* alternateStree = drawableStree->GetAlternateSTree();
    if (originalStree) {
        SetSemiMode__13OriginalSTreei(originalStree, mode);
    }
    if (alternateStree && alternateStree != originalStree) {
        SetSemiMode__13OriginalSTreei(alternateStree, mode);
    }
}

static void SyncFlipTreeWithDrawable(Model* model, AnimStructure* anim) {
    if (!model || !anim || !anim->flip) {
        return;
    }

    DrawableBasic* drawable = model->drawable;
    OriginalSTree* active = GetActiveSTree(drawable);
    STreeData* skeleton = active ? active->skeleton : nullptr;
    if (!skeleton) {
        return;
    }

    if (anim->flip->tree != skeleton) {
        anim->flip->tree = skeleton;
        anim->flip->dirty = 1;
    }
}

static void UpdateFlipMirrorState(Model* model, AnimStructure* anim) {
    if (!anim || !anim->flip) {
        return;
    }

    DrawableSTree* drawable = GetDrawableSTree(model);
    anim->flip->mirrored = drawable && ((drawable->mirrorFlags & 1) != 0);
    anim->flip->mirroredJointOrderMap = drawable ? drawable->mirroredJointOrderMap : nullptr;
}

static TransformAnim* GetTransformAnimForModel(void* animation) {
    if (!animation || IsCameraParamAnim(animation)) {
        return nullptr;
    }

    if (IsCharSequenceAnim(animation)) {
        CharSequenceAnim* seq = static_cast<CharSequenceAnim*>(animation);
        return (seq->parts && seq->numParts > 0) ? seq->parts[0] : nullptr;
    }

    return static_cast<TransformAnim*>(animation);
}

static OriginalSTree* CloneActiveSTree(const OriginalSTree* source) {
    if (!source || !source->skeleton) {
        return nullptr;
    }

    OriginalSTree* clone = new OriginalSTree();
    clone->nameCRC = source->nameCRC;
    clone->SetStoreID(source->GetStoreID());
    clone->meshVertCount = source->meshVertCount;
    clone->meshTriCount = source->meshTriCount;
    clone->skeleton = CloneSTreeData(source->skeleton);
    if (!clone->skeleton) {
        delete clone;
        return nullptr;
    }

    return clone;
}

static constexpr u32 kSuitEntityTypeCompositeAnim = 0x00010001u;
static constexpr u32 kSuitEntityTypeTexList = 0x00010007u;
static constexpr u32 kSuitEntityTypeClutList = 0x00010008u;

static CompositeAnimData* ResolveCompositeAnimRootForSuit(CompositeAnimData* compositeAnim) {
    CompositeAnimData* resolved = compositeAnim;
    for (u32 depth = 0; depth < 8; depth++) {
        if (!resolved || resolved->numParts != 0 || !g_animMgr) {
            break;
        }

        MiscAnimNode* node = g_animMgr->GetMiscAnim(resolved->nameUID);
        if (!node || !node->compositeAnim || node->compositeAnim == resolved) {
            break;
        }

        resolved = node->compositeAnim;
    }

    return resolved;
}

static u32 ResolveCompositePartEntityTypeForSuit(CompositeAnimPartData* part) {
    if (!part) {
        return 0;
    }

    if (part->compositeAnim) {
        return kSuitEntityTypeCompositeAnim;
    }

    if (part->clutAnim) {
        return kSuitEntityTypeClutList;
    }

    if (part->animNameUID == 0 || !g_animMgr) {
        return 0;
    }

    MiscAnimNode* liveNode = g_animMgr->GetMiscAnim(part->animNameUID);
    if (liveNode && liveNode != part->animNode) {
        part->animNode = liveNode;
    }

    if (!part->animNode) {
        return 0;
    }

    if (part->animNode->compositeAnim) {
        part->compositeAnim = part->animNode->compositeAnim;
        return kSuitEntityTypeCompositeAnim;
    }

    if (part->animNode->clutAnim) {
        part->clutAnim = part->animNode->clutAnim;
        return kSuitEntityTypeClutList;
    }

    return 0;
}

// PSX: RedirectCompositeSuitAnimation__FP14tCompositeAnimPC9tPrimGeom (MODEL.CPP:3521)
static s32 RedirectCompositeSuitAnimation(CompositeAnimData* compositeAnim, const tPrimGeom* primGeom) {
    MARKFUNCTION(0x800721B0);

    compositeAnim = ResolveCompositeAnimRootForSuit(compositeAnim);

    if (!compositeAnim || !compositeAnim->parts) {
        return 0;
    }

    for (u32 partIndex = 0; partIndex < compositeAnim->numParts; partIndex++) {
        CompositeAnimPartData& part = compositeAnim->parts[partIndex];
        const u32 entityType = ResolveCompositePartEntityTypeForSuit(&part);
        if (entityType == kSuitEntityTypeCompositeAnim && part.compositeAnim) {
            RedirectCompositeSuitAnimation(part.compositeAnim, primGeom);
        }
        else if (entityType == kSuitEntityTypeTexList || entityType == kSuitEntityTypeClutList) {
            // PSX writes prim geom targets into tex/clut list objects at redirect time.
            part.targetPrimGeom = primGeom;
        }
    }

    return 0;
}

static s32 ResolveSuitPartFrame(const CompositeAnimPartData* part, s32 frame, s32 frameCount) {
    if (!part || frameCount <= 0) {
        return 0;
    }

    const u32 shift = static_cast<u32>(part->field1) & 31u;
    s32 partFrame = frame >> shift;
    if (partFrame >= frameCount) {
        if (part->field0 != 0) {
            partFrame %= frameCount;
        }
        else {
            partFrame = frameCount - 1;
        }
    }

    return partFrame;
}

// cbaLane: vertex 0's CBA value; vertex i's is cbaLane[i * cbaStride] (the
// float vertex layout uses stride 10, the fused upload a compact lane).
static void ApplyClutListSuitFrameToVerts(const ClutAnimData* clutAnim,
                                          s32 frame,
                                          const SkinData* skin,
                                          f32* cbaLane, u32 cbaStride) {
    if (!clutAnim || !skin || !cbaLane || !clutAnim->frames || clutAnim->numFrames <= 0) {
        return;
    }

    const f32 clutWord = static_cast<f32>(clutAnim->GetFrameValue(frame));

    if (clutAnim->mode != 0
        && clutAnim->offsets
        && clutAnim->numOffsets > 0
        && skin->primPacketOffset
        && skin->primPacketSize
        && skin->primStart
        && skin->primVertCount) {
        u32 matchedOffsets = 0;
        for (s32 offsetIndex = 0; offsetIndex < clutAnim->numOffsets; offsetIndex++) {
            const u32 packetOffset = static_cast<u32>(clutAnim->offsets[offsetIndex]);
            u32 firstPrim = 0;
            u32 endPrim = skin->numPrims;
#if defined(RC_PLATFORM_PSP) && !defined(PSP_SUIT_LOOKUP_REFERENCE)
            if (skin->pspPacketRangesOrdered) {
                firstPrim = static_cast<u32>(PspPacketLookup::Find(
                    skin->primPacketOffset, skin->primPacketSize,
                    skin->numPrims, packetOffset));
                endPrim = firstPrim < skin->numPrims ? firstPrim + 1u : firstPrim;
            }
#endif
            for (u32 primIndex = firstPrim; primIndex < endPrim; primIndex++) {
                const u32 packetBase = skin->primPacketOffset[primIndex];
                const u32 packetSize = static_cast<u32>(skin->primPacketSize[primIndex]);
                if (packetSize == 0 || packetOffset < packetBase || packetOffset >= (packetBase + packetSize)) {
                    continue;
                }

                const u32 start = skin->primStart[primIndex];
                const u32 count = static_cast<u32>(skin->primVertCount[primIndex]);
                for (u32 corner = 0; corner < count; corner++) {
                    const u32 vertexIndex = start + corner;
                    if (vertexIndex >= skin->numVerts) {
                        break;
                    }

                    cbaLane[vertexIndex * cbaStride] = clutWord;
                }

                matchedOffsets++;
                break;
            }
        }

        return;
    }

    for (u32 vertexIndex = 0; vertexIndex < skin->numVerts; vertexIndex++) {
        cbaLane[vertexIndex * cbaStride] = clutWord;
    }
}

static void ApplyCompositeSuitFrameToVerts(CompositeAnimData* compositeAnim,
                                           s32 frame,
                                           const SkinData* skin,
                                           f32* cbaLane, u32 cbaStride) {
    compositeAnim = ResolveCompositeAnimRootForSuit(compositeAnim);

    if (!compositeAnim || !compositeAnim->parts || !skin || !cbaLane) {
        return;
    }

    u32 resolvedCompositeParts = 0;
    u32 resolvedClutParts = 0;
    u32 unresolvedParts = 0;

    for (u32 partIndex = 0; partIndex < compositeAnim->numParts; partIndex++) {
        CompositeAnimPartData& part = compositeAnim->parts[partIndex];
        const u32 entityType = ResolveCompositePartEntityTypeForSuit(&part);

        if (entityType == kSuitEntityTypeCompositeAnim && part.compositeAnim) {
            resolvedCompositeParts++;
            const s32 partFrame = ResolveSuitPartFrame(&part, frame, static_cast<s32>(part.compositeAnim->field12));
            ApplyCompositeSuitFrameToVerts(part.compositeAnim, partFrame, skin, cbaLane, cbaStride);
        }
        else if (entityType == kSuitEntityTypeClutList && part.clutAnim) {
            resolvedClutParts++;
            const s32 partFrame = ResolveSuitPartFrame(&part, frame, part.clutAnim->numFrames);
            ApplyClutListSuitFrameToVerts(part.clutAnim, partFrame, skin, cbaLane, cbaStride);
        }
        else {
            unresolvedParts++;
        }
    }
}

#if defined(RC_PLATFORM_PSP) && defined(RC_PSP_VFPU_SKIN) && !defined(PSP_DISABLE_FUSED_SKIN_UPLOAD)
namespace {

// Suit CBA lanes for the fused character upload. A lane depends only on the
// skin, the suit, the suit index and the misc anims the suit resolves through
// (ApplyCompositeSuitFrameToVerts above), so each combination is built once
// and handed to the upload under its own lane id: a buffer that already
// holds that lane skips the per-vertex material comparison.
struct PspSuitLane {
    u32 skinId = 0;
    const CompositeAnimData* suit = nullptr;
    u32 suitIndex = 0;
    u32 animGeneration = 0;
    u32 laneId = 0;  // 0: empty slot
    u32 lastUse = 0;
    std::vector<f32> cba;
};
PspSuitLane g_pspSuitLanes[8];
u32 g_pspSuitLaneClock = 0;

void BuildPspSuitLane(CompositeAnimData* suit, u32 suitIndex, const SkinData* skin,
                      std::vector<f32>& cba) {
    ResizeRetainedScratch(cba, skin->numVerts);
    for (u32 i = 0; i < skin->numVerts; i++) {
        cba[i] = skin->verts[i].cba;
    }
    ApplyCompositeSuitFrameToVerts(suit, static_cast<s32>(suitIndex), skin, cba.data(), 1u);
}

const PspSuitLane& AcquirePspSuitLane(CompositeAnimData* suit, u32 suitIndex,
                                      const SkinData* skin) {
    const u32 generation = g_animMgr ? g_animMgr->generation : 0u;
    PspSuitLane* victim = &g_pspSuitLanes[0];
    for (PspSuitLane& lane : g_pspSuitLanes) {
        if (lane.laneId != 0 && lane.skinId == skin->pspInstanceId && lane.suit == suit
            && lane.suitIndex == suitIndex && lane.animGeneration == generation) {
            lane.lastUse = ++g_pspSuitLaneClock;
#if defined(PSP_PERF_TELEMETRY)
            // Benchmarks re-check a cache hit now and then against a fresh
            // build; any difference means the key misses an input.
            if ((g_pspSuitLaneClock & 63u) == 0u) {
                static std::vector<f32> fresh;
                BuildPspSuitLane(suit, suitIndex, skin, fresh);
                if (fresh.size() != lane.cba.size()
                    || std::memcmp(fresh.data(), lane.cba.data(), fresh.size() * sizeof(f32)) != 0) {
                    PspPerf::CountRenderer(PspPerf::RendererCounter::SuitLaneMismatches);
                }
            }
#endif
            return lane;
        }
        if (lane.lastUse < victim->lastUse) {
            victim = &lane;
        }
    }
    PspPerf::CountRenderer(PspPerf::RendererCounter::SuitLaneBuilds);
    BuildPspSuitLane(suit, suitIndex, skin, victim->cba);
    victim->skinId = skin->pspInstanceId;
    victim->suit = suit;
    victim->suitIndex = suitIndex;
    victim->animGeneration = generation;
    victim->laneId = NextPspSkinInstanceId();  // never reused, never 0
    victim->lastUse = ++g_pspSuitLaneClock;
    return *victim;
}

} // namespace
#endif


static const Mat4* FindJointWorldMatrixByHash(const STreeData* skeleton,
                                              const Mat4* jointMatrices,
                                              u32 jointHash) {
    if (!skeleton || !jointMatrices || !skeleton->joints || jointHash == 0) {
        return nullptr;
    }

    for (u32 jointIndex = 0; jointIndex < skeleton->numJoints; jointIndex++) {
        if (skeleton->joints[jointIndex].nameUID == jointHash) {
            return &jointMatrices[jointIndex];
        }
    }

    return nullptr;
}

static bool CanLightGeo(const OriginalGeo* geo) {
    return geo && geo->dynamicVerts && geo->dynamicVertCount != 0 && geo->dynamicVertSourceIndex
        && geo->primGeom && geo->primGeom->GetVertexList();
}

static Mat4 GetGeoLightingWorld() {
    const Mat4 world = p3d::context ? p3d::context->GetWorldMatrix() : Mat4();
    Mat4 lightingWorld = world;
    const f32 basisXLen = std::sqrt(
        lightingWorld.m[0] * lightingWorld.m[0]
        + lightingWorld.m[1] * lightingWorld.m[1]
        + lightingWorld.m[2] * lightingWorld.m[2]);

    if (basisXLen > 0.00001f) {
        const f32 invScale = 1.0f / basisXLen;
        lightingWorld.m[0] *= invScale;
        lightingWorld.m[1] *= invScale;
        lightingWorld.m[2] *= invScale;
        lightingWorld.m[4] *= invScale;
        lightingWorld.m[5] *= invScale;
        lightingWorld.m[6] *= invScale;
        lightingWorld.m[8] *= invScale;
        lightingWorld.m[9] *= invScale;
        lightingWorld.m[10] *= invScale;
    }
    return lightingWorld;
}

// Lights one dynamic vertex in place (r/g/b, PSX fixed point / 128). Vertices
// without a valid source normal keep their base colour.
static void LightGeoVertexColour(const OriginalGeo* geo, const u8* sourceVerts,
                                 const Mat4& lightingWorld, u32 i,
                                 f32* r, f32* g, f32* b) {
    const u32 sourceIndex = static_cast<u32>(geo->dynamicVertSourceIndex[i]);
    if (sourceIndex >= geo->primGeom->numVerts) {
        return;
    }

    const u8* sourceVertex = sourceVerts + sourceIndex * 8u;
    const u8 normalIndex = static_cast<u8>((p3dReadU32LE(sourceVertex + 4u) >> 16) & 0xFFu);

    u8 litR = 0;
    u8 litG = 0;
    u8 litB = 0;
    ComputePsxLitColourFromNormalIndex(normalIndex, &lightingWorld, &litR, &litG, &litB);
    const u8 litI = static_cast<u8>((static_cast<u32>(litR)
        + static_cast<u32>(litG)
        + static_cast<u32>(litB)
        + 1u) / 3u);

    u32 baseR = static_cast<u32>(*r * 128.0f + 0.5f);
    u32 baseG = static_cast<u32>(*g * 128.0f + 0.5f);
    u32 baseB = static_cast<u32>(*b * 128.0f + 0.5f);
    if (baseR > 255u) baseR = 255u;
    if (baseG > 255u) baseG = 255u;
    if (baseB > 255u) baseB = 255u;

    *r = static_cast<f32>(ModulatePsxColourChannel(static_cast<u8>(baseR), litI)) / 128.0f;
    *g = static_cast<f32>(ModulatePsxColourChannel(static_cast<u8>(baseG), litI)) / 128.0f;
    *b = static_cast<f32>(ModulatePsxColourChannel(static_cast<u8>(baseB), litI)) / 128.0f;
}

static bool BuildLitGeoVerts(OriginalGeo* geo, std::vector<GeoRenderVertex>& outVerts) {
    if (!CanLightGeo(geo)) {
        return false;
    }

    const u8* sourceVerts = geo->primGeom->GetVertexList();
    outVerts.assign(geo->dynamicVerts, geo->dynamicVerts + geo->dynamicVertCount);
    const Mat4 lightingWorld = GetGeoLightingWorld();
    for (u32 i = 0; i < geo->dynamicVertCount; i++) {
        LightGeoVertexColour(geo, sourceVerts, lightingWorld, i,
                             &outVerts[i].r, &outVerts[i].g, &outVerts[i].b);
    }

    return true;
}

#if defined(RC_PLATFORM_PSP)
// A lit PSX channel (0..255, 128 = 1.0) as the GU vertex byte, exactly what
// MakePspGeometryColour(value / 128.0f) produces: value / 128 * 255 is exact
// in single precision for these integers, so its clamp-and-truncate equals
// floor(value * 255 / 128), saturating from 128 up.
static u32 PsxLitChannelToGuByte(u32 value) {
    return value >= 128u ? 255u : (value * 255u) >> 7;
}

// Same lighting as BuildLitGeoVerts, emitted directly as GU vertex colours so
// the renderer can patch only the colour lane of the shared prop buffer.
static bool BuildLitGeoColours(OriginalGeo* geo, std::vector<u32>& outColours) {
    if (!CanLightGeo(geo)) {
        return false;
    }

    const u8* sourceVerts = geo->primGeom->GetVertexList();
    const u32 sourceCount = geo->primGeom->numVerts;
    ResizeRetainedScratch(outColours, geo->dynamicVertCount);
    const Mat4 lightingWorld = GetGeoLightingWorld();
    // The lit intensity depends only on the normal index while the lights and
    // this matrix stay fixed, so each of the <= 256 normals is lit once per
    // call instead of once per vertex (LightGeoVertexColour's per-vertex work).
    u8 litByNormal[256];
    u32 litKnown[8] = {};
    for (u32 i = 0; i < geo->dynamicVertCount; i++) {
        const auto& vertex = geo->dynamicVerts[i];
        const u32 sourceIndex = static_cast<u32>(geo->dynamicVertSourceIndex[i]);
        if (sourceIndex >= sourceCount) {
            outColours[i] = MakePspGeometryColour(vertex.r, vertex.g, vertex.b);
            continue;
        }

        const u8* sourceVertex = sourceVerts + sourceIndex * 8u;
        const u8 normalIndex = static_cast<u8>((p3dReadU32LE(sourceVertex + 4u) >> 16) & 0xFFu);
        const u32 knownBit = 1u << (normalIndex & 31u);
        if ((litKnown[normalIndex >> 5] & knownBit) == 0u) {
            u8 litR = 0;
            u8 litG = 0;
            u8 litB = 0;
            ComputePsxLitColourFromNormalIndex(normalIndex, &lightingWorld, &litR, &litG, &litB);
            litByNormal[normalIndex] = static_cast<u8>((static_cast<u32>(litR)
                + static_cast<u32>(litG)
                + static_cast<u32>(litB)
                + 1u) / 3u);
            litKnown[normalIndex >> 5] |= knownBit;
        }
        const u8 litI = litByNormal[normalIndex];
        const auto litChannel = [litI](f32 channel) {
            u32 base = static_cast<u32>(channel * 128.0f + 0.5f);
            if (base > 255u) base = 255u;
            return PsxLitChannelToGuByte(ModulatePsxColourChannel(static_cast<u8>(base), litI));
        };
        outColours[i] = litChannel(vertex.r)
            | (litChannel(vertex.g) << 8)
            | (litChannel(vertex.b) << 16)
            | 0xFF000000u;
    }

    return true;
}
#endif

static void DrawGeoPartMesh(OriginalGeo* geo, bool applyLighting) {
    if (!geo || !geo->meshBuffer) {
        return;
    }

#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    static u32 pspGeoDrawProbe = 0;
    const u32 pspGeoDrawProbeId = pspGeoDrawProbe++;
    const bool pspGeoProbe = pspGeoDrawProbeId < 64u;
    if (pspGeoProbe) {
        LOG("[PSP GeoDraw] begin #%lu geo=%p mesh=%p lighting=%d dyn=%lu",
            static_cast<unsigned long>(pspGeoDrawProbeId), static_cast<void*>(geo),
            static_cast<void*>(geo->meshBuffer), applyLighting ? 1 : 0,
            static_cast<unsigned long>(geo->dynamicVertCount));
    }
#else
    const bool pspGeoProbe = false;
#endif

    p3d::context->SetCullMode(PDDI_CULL_NONE);
    if (pspGeoProbe) {
        LOG("[PSP GeoDraw] after cull");
    }

    if (geo->usesSemiTrans) {
        pddiBlendMode blendMode = PDDI_BLEND_ALPHA;
        switch (geo->semiTransMode & 3u) {
            case 1: blendMode = PDDI_BLEND_ADD; break;
            case 2: blendMode = PDDI_BLEND_SUBTRACT; break;
            case 3: blendMode = PDDI_BLEND_PSX_QUARTER; break;
            default: break;
        }
        p3d::context->SetBlendMode(blendMode);
    }
    if (pspGeoProbe) {
        LOG("[PSP GeoDraw] after blend");
    }

#if defined(RC_PLATFORM_PSP)
    std::vector<GeoRenderVertex>& litVerts = g_pspModelFrameScratch.litGeoVertices;
    litVerts.clear();
    // Lighting changes only the colours of the shared prop buffer: patch that
    // lane instead of a full re-upload. The patch stays after the draw, so a
    // prop under steady light patches nothing next frame and its buffer stays
    // static (a recorded GE list); the renderer restores the base colours
    // before any other draw of the buffer.
    std::vector<u32>& litColours = g_pspModelFrameScratch.litGeoColours;
    const bool pspColoursPatched = applyLighting
        && BuildLitGeoColours(geo, litColours)
        && PatchPspPrimBufferColours(geo->meshBuffer, litColours.data(),
                                     geo->dynamicVertCount);
#else
    std::vector<GeoRenderVertex> litVerts;
    constexpr bool pspColoursPatched = false;
#endif
    if (applyLighting && !pspColoursPatched && BuildLitGeoVerts(geo, litVerts)) {
#if defined(RC_PLATFORM_PSP)
        if (!UpdatePspPrimBufferDynamicVertices(geo->meshBuffer,
                                                litVerts.data(),
                                                geo->dynamicVertCount))
#endif
        {
#if defined(RC_PLATFORM_PSP)
            // The renderer fast path is strict about the shared 10-float
            // layout; retain the generic conversion as a safe fallback.
#endif
            geo->meshBuffer->SetVertexData(litVerts.data(), geo->dynamicVertCount);
        }
    }
    if (pspGeoProbe) {
        LOG("[PSP GeoDraw] after lit verts");
    }

#if defined(RC_PLATFORM_PSP)
    if (pspColoursPatched) BeginPspLitColourDraw(geo->meshBuffer);
#endif
    DrawTexturedMesh(geo->meshBuffer
#if REAL_TEXTURE_RENDERING
                      , &geo->realTexGroups
#endif
                      );
#if defined(RC_PLATFORM_PSP)
    if (pspColoursPatched) EndPspLitColourDraw();
#endif
    if (pspGeoProbe) {
        LOG("[PSP GeoDraw] after draw");
    }
    if (!litVerts.empty()) {
        geo->meshBuffer->SetVertexData(geo->dynamicVerts, geo->dynamicVertCount);
    }
    if (pspGeoProbe) {
        LOG("[PSP GeoDraw] after restore verts");
    }

    if (geo->usesSemiTrans) {
        p3d::context->SetBlendMode(PDDI_BLEND_NONE);
    }
}

CompositeAnimData::~CompositeAnimData() {
    delete[] parts;
    if (compositeAnims) {
        for (u32 i = 0; i < numCompositeAnims; i++) {
            delete compositeAnims[i];
        }
        delete[] compositeAnims;
    }
    if (clutAnims) {
        for (u32 i = 0; i < numClutAnims; i++) {
            delete clutAnims[i];
        }
        delete[] clutAnims;
    }
}

// OriginalSTree
OriginalSTree::OriginalSTree() {
    SetType(1); // STree type
    RegisterLiveOriginalSTree(this);
}

OriginalSTree::~OriginalSTree() {
    UnregisterLiveOriginalSTree(this);

    if (primGeom) {
        delete primGeom;
        primGeom = nullptr;
    }

    if (meshBuffer) {
        meshBuffer->Release();
        meshBuffer = nullptr;
    }
    if (skeleton) {
        delete skeleton;
        skeleton = nullptr;
    }
    if (skinData) {
        delete skinData;
        skinData = nullptr;
    }
    if (compositeAnim) {
        delete compositeAnim;
        compositeAnim = nullptr;
    }
}

OriginalGeo::OriginalGeo() {
    SetType(0); // Geo type
}

OriginalGeo::~OriginalGeo() {
    if (primGeom) {
        delete primGeom;
        primGeom = nullptr;
    }

    if (meshBuffer) {
        meshBuffer->Release();
        meshBuffer = nullptr;
    }

    delete[] dynamicVerts;
    dynamicVerts = nullptr;
    dynamicVertCount = 0;

    delete[] dynamicVertSourceIndex;
    dynamicVertSourceIndex = nullptr;

    delete[] dynamicColorList;
    dynamicColorList = nullptr;
    dynamicColorCount = 0;

    delete[] dynamicPrimStart;
    dynamicPrimStart = nullptr;

    delete[] dynamicPrimVertCount;
    dynamicPrimVertCount = nullptr;

    delete[] dynamicPrimMaterialUID;
    dynamicPrimMaterialUID = nullptr;

    delete[] dynamicPrimCmd;
    dynamicPrimCmd = nullptr;

    delete[] dynamicPrimUVWords;
    dynamicPrimUVWords = nullptr;

    delete[] dynamicPrimPacketOffset;
    dynamicPrimPacketOffset = nullptr;

    dynamicPrimCount = 0;
}

OriginalETree::OriginalETree() {
    SetType(2); // ETree type
}

OriginalETree::~OriginalETree() {
    if (meshBuffer) {
        meshBuffer->Release();
        meshBuffer = nullptr;
    }

    if (skeleton) {
        delete skeleton;
        skeleton = nullptr;
    }

    delete[] geoParts;
    geoParts = nullptr;

    delete[] geoPartJointHashes;
    geoPartJointHashes = nullptr;

    delete[] geoPartHashes;
    geoPartHashes = nullptr;

    geoPartCount = 0;
}

DrawableBasic::~DrawableBasic() = default;

// DrawableSTree
DrawableSTree::DrawableSTree(OriginalSTree* orig) {
    original = orig;
    if (orig && orig->skeleton && orig->activeTreeInUse) {
        alternate = CloneActiveSTree(orig);
    }

    if (!alternate) {
        alternate = orig;
        if (orig) {
            orig->activeTreeInUse = true;
        }
    }

    mirrorFlags = 0;
    mirroredJointOrderMap = nullptr;
    suitIndex = 0;
}

DrawableSTree::~DrawableSTree() {
    if (alternate && alternate != original) {
        delete alternate;
    }
    else if (original && IsLiveOriginalSTree(original)) {
        original->activeTreeInUse = false;
    }

    original = nullptr;
    alternate = nullptr;
    delete[] mirroredJointOrderMap;
    mirroredJointOrderMap = nullptr;
}

// PSX: Display dispatches through vtable to OriginalSTree::Draw -> tPrimGeom::Display
// PC: draws the skeleton mesh (per-joint transforms baked in) or flat fallback

void DrawableSTree::Display(u32 /*flags*/) {
    OriginalSTree* active = GetActiveSTree(this);
    OriginalSTree* renderSource = original ? original : active;
    if (!active || !renderSource) {
        return;
    }

    STreeData* skel = active->skeleton;
    SkinData* skin = renderSource->skinData;
    STreeData* renderSkeleton = renderSource->skeleton;
    pddiPrimBuffer* skinnedBuffer = (renderSkeleton && renderSkeleton->joints && renderSkeleton->numJoints > 0)
        ? renderSkeleton->joints[0].meshBuffer
        : nullptr;

    static std::set<const void*> sDbgDisp;
    if (sDbgDisp.size() < 64 && sDbgDisp.insert(this).second) {
        u32 distinctJoints = 0;
        s32 maxJoint = -1;
        s32 minJoint = 0x7FFFFFFF;
        bool seen[256] = {};
        u32 numJoints = skel ? skel->numJoints : 0u;
        if (skin && skin->verts) {
            for (u32 vi = 0; vi < skin->numVerts; vi++) {
                const s32 j = skin->verts[vi].jointIdx;
                if (j >= 0 && j < 256 && !seen[j]) {
                    seen[j] = true;
                    distinctJoints++;
                }
                if (j > maxJoint) maxJoint = j;
                if (j < minJoint) minJoint = j;
            }
        }
        LOG("[Disp] dtree=%p active=%p orig=%p skel=%p skin=%p nv=%u rskel=%p skinnedBuf=%p numJoints=%u distinctJoints=%u jmin=%d jmax=%d",
            static_cast<const void*>(this), static_cast<const void*>(active),
            static_cast<const void*>(original), static_cast<const void*>(skel),
            static_cast<const void*>(skin), skin ? skin->numVerts : 0u,
            static_cast<const void*>(renderSkeleton), static_cast<const void*>(skinnedBuffer),
            numJoints, distinctJoints, minJoint, maxJoint);
    }

    auto resolveSemiTransBlendMode = [](u8 semiTransMode) -> pddiBlendMode {
        switch (semiTransMode & 3u) {
            case 1: return PDDI_BLEND_ADD;
            case 2: return PDDI_BLEND_SUBTRACT;
            case 3: return PDDI_BLEND_PSX_QUARTER;
            default: return PDDI_BLEND_ALPHA;
        }
    };

    auto applySkinBlendState = [&](const SkinData* skinData) -> bool {
        if (!skinData || !skinData->usesSemiTrans) {
            return false;
        }

        p3d::context->SetBlendMode(resolveSemiTransBlendMode(skinData->semiTransMode));
        return true;
    };

    // Per-frame CPU skinning
    if (skel && skin && skin->numVerts > 0 && skel->joints && skinnedBuffer) {
        PspPerf::ScopeTimer skinTimer(PspPerf::RendererCounter::SkinPrepareUs);
#if defined(RC_PLATFORM_PSP)
        // Joint world matrices: written once here, then read by the view
        // cull, once per joint by the light pass and at every joint change of
        // the skin pass, after the skin stream has evicted them from the data
        // cache. In the scratchpad joints region (psp/psp_scratchpad.h) they
        // cost no line fill on write, no write-back and no refill; a skeleton
        // above the region capacity, or a nested draw, keeps the RAM vector.
        PspScratchpad::JointsLease jointsLease(skel->numJoints);
        Mat4* jointData = static_cast<Mat4*>(jointsLease.data());
        if (!jointData) {
            ResizeRetainedScratch(g_pspModelFrameScratch.jointMatrices, skel->numJoints);
            jointData = g_pspModelFrameScratch.jointMatrices.data();
        }
        skel->ComputeWorldMatricesWithCallbacks(jointData);
#else
        Mat4* jointMatrices = new Mat4[skel->numJoints];
        skel->ComputeWorldMatricesWithCallbacks(jointMatrices);
#endif
        const Mat4 modelWorld = p3d::context->GetWorldMatrix();
#if defined(RC_PLATFORM_PSP)
        // Joint callbacks above still ran (attack capture, overrides). Only
        // the purely visual light/skin/pack/draw work is skipped, and only
        // when no vertex can reach the screen.
        if (PspSkinOutsideView(skin, jointData, skel->numJoints, modelWorld)) {
            PspPerf::CountRenderer(PspPerf::RendererCounter::SkinViewCulls);
            return;
        }
        PspPerf::CountRenderer(PspPerf::RendererCounter::SkinnedVertices, skin->numVerts);
#endif
        Mat4 lightingWorld = modelWorld;
        {
            const f32 basisXLen = std::sqrt(
                lightingWorld.m[0] * lightingWorld.m[0]
                + lightingWorld.m[1] * lightingWorld.m[1]
                + lightingWorld.m[2] * lightingWorld.m[2]);

            if (basisXLen > 0.00001f) {
                const f32 invScale = 1.0f / basisXLen;
                lightingWorld.m[0] *= invScale;
                lightingWorld.m[1] *= invScale;
                lightingWorld.m[2] *= invScale;
                lightingWorld.m[4] *= invScale;
                lightingWorld.m[5] *= invScale;
                lightingWorld.m[6] *= invScale;
                lightingWorld.m[8] *= invScale;
                lightingWorld.m[9] *= invScale;
                lightingWorld.m[10] *= invScale;
            }
        }

#if defined(RC_PLATFORM_PSP)
        std::vector<u16>& litScratch = g_pspModelFrameScratch.litScratch;
        std::vector<u8>& litWritten = g_pspModelFrameScratch.litWritten;
        litScratch.clear();
        litWritten.clear();
#else
        std::vector<u16> litScratch;
        std::vector<u8> litWritten;
#endif
        bool hasLitScratch = false;
        u8 scratchMinR = 255;
        u8 scratchMinG = 255;
        u8 scratchMinB = 255;
        u8 scratchMaxR = 0;
        u8 scratchMaxG = 0;
        u8 scratchMaxB = 0;
        bool haveScratchRange = false;
        u8 normalIndexMin = 255;
        u8 normalIndexMax = 0;
        bool haveNormalIndexRange = false;
#if defined(RC_PSP_VFPU_SKIN)
        // The first character draw with a lighting callback runs both paths
        // and compares them; it draws the scalar results.
        const bool vfpuSelfTest = g_pspVfpuSkinMode == PspVfpuSkinMode::Untested
            && renderSource->xformVertsCallback && renderSource->primGeom
            && renderSource->primGeom->numVerts > 0;
        const bool vfpuEnabled = g_pspVfpuSkinMode == PspVfpuSkinMode::Enabled;
#if defined(PSP_NORMAL_LIGHT_CACHE)
        // The normal-cache experiment lives in the callback; keep measuring it.
        const bool vfpuLight = false;
#else
        const bool vfpuLight = (vfpuSelfTest || vfpuEnabled)
            && renderSource->xformVertsCallback == RP_XformVertsLitCBF_CL;
#endif
        PspVfpuSkinSelfTest vfpuTest;
#endif
        if (renderSource->xformVertsCallback && renderSource->primGeom && renderSource->primGeom->numVerts > 0) {
            const u32 sourceVertCount = renderSource->primGeom->numVerts;
            const u8* sourceVertexList = renderSource->primGeom->GetVertexList();
            ResizeRetainedScratch(litScratch, sourceVertCount * 2u);
            ResizeRetainedScratch(litWritten, sourceVertCount);
            std::fill(litScratch.begin(), litScratch.end(), 0u);
            std::fill(litWritten.begin(), litWritten.end(), 0u);

#if defined(RC_PSP_VFPU_SKIN)
            std::vector<u16> vfpuTestLit;
            if (vfpuLight) {
                u16* target = litScratch.data();
                if (vfpuSelfTest) {
                    vfpuTestLit.assign(sourceVertCount * 2u, 0u);
                    target = vfpuTestLit.data();
                }
                const s32 lastJoint = PspLightJointsVfpu(renderSource, skel, jointData,
                                                         lightingWorld, sourceVertCount, target,
                                                         litWritten.data());
                if (!vfpuSelfTest && lastJoint >= 0) {
                    // Keep the callback's side effects (lit-scratch owner used
                    // by RP_FixUpPolysCBF_CL, light-state trace) without
                    // lighting any vertex a second time.
                    STreeJoint emptyJoint = skel->joints[lastJoint];
                    emptyJoint.primGeomCount = 0;
                    renderSource->xformVertsCallback(renderSource->primGeom, &emptyJoint,
                                                     reinterpret_cast<u32*>(&lightingWorld),
                                                     litScratch.data());
                }
            }
            if (!vfpuLight || vfpuSelfTest) {
#endif
            for (u32 jointIndex = 0; jointIndex < skel->numJoints; jointIndex++) {
                STreeJoint* joint = &skel->joints[jointIndex];
                if (joint->primGeomCount == 0) {
                    continue;
                }

                const u32 startIndex = static_cast<u32>(joint->primGeomStartIdx);
                const u32 endIndex = startIndex + static_cast<u32>(joint->primGeomCount);
                if (startIndex >= sourceVertCount) {
                    continue;
                }
                const u32 clampedEnd = (endIndex > sourceVertCount) ? sourceVertCount : endIndex;
                if (clampedEnd <= startIndex) {
                    continue;
                }
                for (u32 sourceIndex = startIndex; sourceIndex < clampedEnd; sourceIndex++) {
                    litWritten[sourceIndex] = 1u;
                    if (sourceVertexList) {
                        const u8 normalIndex = sourceVertexList[sourceIndex * 8u + 6u];
                        if (!haveNormalIndexRange) {
                            normalIndexMin = normalIndex;
                            normalIndexMax = normalIndex;
                            haveNormalIndexRange = true;
                        }
                        else {
                            if (normalIndex < normalIndexMin) normalIndexMin = normalIndex;
                            if (normalIndex > normalIndexMax) normalIndexMax = normalIndex;
                        }
                    }
                }

                STreeJoint clampedJoint = *joint;
                clampedJoint.primGeomStartIdx = static_cast<u16>(startIndex);
                clampedJoint.primGeomCount = static_cast<u16>(clampedEnd - startIndex);
#if defined(RC_PLATFORM_PSP)
                Mat4 lightingMatrix = lightingWorld * jointData[jointIndex];
#else
                Mat4 lightingMatrix = lightingWorld * jointMatrices[jointIndex];
#endif
                renderSource->xformVertsCallback(renderSource->primGeom,
                                                 &clampedJoint,
                                                 reinterpret_cast<u32*>(&lightingMatrix),
                                                 litScratch.data());
            }
#if defined(RC_PSP_VFPU_SKIN)
            }
            if (vfpuLight && vfpuSelfTest) {
                vfpuTest.CompareLight(litScratch.data(), vfpuTestLit.data(), litWritten.data(),
                                      sourceVertCount);
            }
#endif

            hasLitScratch = true;
            for (u32 sourceIndex = 0; sourceIndex < sourceVertCount; sourceIndex++) {
                if (litWritten[sourceIndex] == 0u) {
                    continue;
                }

                const u16 rg = litScratch[sourceIndex * 2u + 0u];
                const u16 b = litScratch[sourceIndex * 2u + 1u];
                const u8 litR = static_cast<u8>(rg & 0xFFu);
                const u8 litG = static_cast<u8>((rg >> 8) & 0xFFu);
                const u8 litB = static_cast<u8>(b & 0xFFu);

                if (!haveScratchRange) {
                    scratchMinR = litR;
                    scratchMinG = litG;
                    scratchMinB = litB;
                    scratchMaxR = litR;
                    scratchMaxG = litG;
                    scratchMaxB = litB;
                    haveScratchRange = true;
                }
                else {
                    if (litR < scratchMinR) scratchMinR = litR;
                    if (litG < scratchMinG) scratchMinG = litG;
                    if (litB < scratchMinB) scratchMinB = litB;
                    if (litR > scratchMaxR) scratchMaxR = litR;
                    if (litG > scratchMaxG) scratchMaxG = litG;
                    if (litB > scratchMaxB) scratchMaxB = litB;
                }
            }
        }

#if defined(RC_PLATFORM_PSP) && defined(RC_PSP_VFPU_SKIN) && !defined(PSP_DISABLE_FUSED_SKIN_UPLOAD)
        // Characters (VFPU enabled, byte colours): skin straight into the
        // buffer's compact stream; see PspSkinPackedVfpu. Anything else, and
        // the self-test draw, keeps the float vertex path below.
        PspPackedVertex* fusedVertices = (vfpuEnabled && !g_pspFusedSkinDisabled
                                          && PspSkinColoursAreBytes(skin))
            ? BeginPspPrimBufferPackedUpload(skinnedBuffer, skin->numVerts)
            : nullptr;
        if (fusedVertices) {
            const PspSkinLoop loop = {
                skin->verts,
                jointData,
                litScratch.data(),
                litWritten.data(),
                (hasLitScratch && renderSource->primGeom)
                    ? static_cast<u32>(renderSource->primGeom->numVerts) : 0u,
            };
            {
                PspPerf::ScopeTimer kernelTimer(PspPerf::RendererCounter::SkinKernelUs);
                if (const PspSkinStreamVertex* stream = AcquirePspSkinStream(skin)) {
                    PspSkinPackedVfpu(loop, stream, skin->numVerts, fusedVertices);
                }
                else {
                    PspSkinPackedVfpu(loop, skin->verts, skin->numVerts, fusedVertices);
                }
            }
            // Material lanes: tpage straight from the skin, CBA from the skin
            // or from a compact copy the suit animation rewrites.
            constexpr u32 kSkinVertexFloats = sizeof(SkinVertex) / sizeof(f32);
            const f32* tpageLane = &skin->verts[0].tpage;
            u32 tpageStride = kSkinVertexFloats;
            const f32* cbaLane = &skin->verts[0].cba;
            u32 cbaStride = kSkinVertexFloats;
            CompositeAnimData* suit = renderSource ? renderSource->compositeAnim : nullptr;
            u32 suitLaneId = 0;
            if (suit) {
                PspPerf::ScopeTimer suitTimer(PspPerf::RendererCounter::SkinSuitUs);
                const PspSuitLane& suitLane = AcquirePspSuitLane(suit, suitIndex, skin);
                cbaLane = suitLane.cba.data();
                cbaStride = 1u;
                suitLaneId = suitLane.laneId;
            }
            // On-device equivalence check, like the VFPU self-test: the first
            // uploads also run the float path and compare every byte. Any
            // difference keeps that draw's reference result and turns the
            // fused path off for the rest of the run.
            if (g_pspFusedSkinChecksLeft > 0u) {
                --g_pspFusedSkinChecksLeft;
                std::vector<f32>& reference = g_pspModelFrameScratch.skinVertexData;
                ResizeRetainedScratch(reference, skin->numVerts * 10u);
                PspSkinVerticesVfpu<true>(loop, 0, skin->numVerts, reference.data());
                ApplyCompositeSuitFrameToVerts(suit, static_cast<s32>(suitIndex), skin,
                                               reference.data() + 9, 10u);
                bool matched = true;
                for (u32 i = 0; i < skin->numVerts && matched; i++) {
                    const f32* in = reference.data() + i * 10u;
                    PspPackedVertex expected;
                    expected.u = static_cast<s16>(in[6]);
                    expected.v = static_cast<s16>(in[7]);
                    expected.color = MakePspGeometryColour(in[3], in[4], in[5]);
                    expected.x = in[0];
                    expected.y = in[1];
                    expected.z = in[2];
                    matched = std::memcmp(&expected, &fusedVertices[i], sizeof(expected)) == 0
                        && std::memcmp(&in[8], tpageLane + i * tpageStride, sizeof(f32)) == 0
                        && std::memcmp(&in[9], cbaLane + i * cbaStride, sizeof(f32)) == 0;
                }
                PspPerf::RecordFusedSkinCheck(matched);
                if (!matched) {
                    g_pspFusedSkinDisabled = true;
                    for (u32 i = 0; i < skin->numVerts; i++) {
                        const f32* in = reference.data() + i * 10u;
                        fusedVertices[i].u = static_cast<s16>(in[6]);
                        fusedVertices[i].v = static_cast<s16>(in[7]);
                        fusedVertices[i].color = MakePspGeometryColour(in[3], in[4], in[5]);
                        fusedVertices[i].x = in[0];
                        fusedVertices[i].y = in[1];
                        fusedVertices[i].z = in[2];
                    }
                    tpageLane = reference.data() + 8;
                    tpageStride = 10u;
                    cbaLane = reference.data() + 9;
                    cbaStride = 10u;
                }
            }
            // Lanes straight from the skin or from a built suit lane are
            // immutable under their id: the buffer may skip re-comparing them.
            // A check mismatch above switched to the reference lanes (no id).
            u32 laneSource = 0;
            if (tpageLane == &skin->verts[0].tpage) {
                laneSource = cbaLane == &skin->verts[0].cba ? skin->pspInstanceId : suitLaneId;
            }
            EndPspPrimBufferPackedUpload(skinnedBuffer, tpageLane, tpageStride,
                                         cbaLane, cbaStride, skin->numVerts, laneSource);
        }
        else
#endif
        {
#if defined(RC_PLATFORM_PSP)
            std::vector<f32>& vertData = g_pspModelFrameScratch.skinVertexData;
            ResizeRetainedScratch(vertData, skin->numVerts * 10u);
#if defined(PSP_SKIN_TRANSFORM_CACHE)
            g_pspModelFrameScratch.transforms.Begin();
#endif
#else
            std::vector<f32> vertData(skin->numVerts * 10);
#endif
#if defined(RC_PLATFORM_PSP)
            {
                const PspSkinLoop loop = {
                    skin->verts,
                    jointData,
                    litScratch.data(),
                    litWritten.data(),
                    (hasLitScratch && renderSource->primGeom)
                        ? static_cast<u32>(renderSource->primGeom->numVerts) : 0u,
                };
#if defined(RC_PSP_VFPU_SKIN)
                if (vfpuEnabled) {
                    if (PspSkinColoursAreBytes(skin)) {
                        PspSkinVerticesVfpu<true>(loop, 0, skin->numVerts, vertData.data());
                    }
                    else {
                        PspSkinVerticesVfpu<false>(loop, 0, skin->numVerts, vertData.data());
                    }
                }
                else
#endif
                {
                    PspSkinVerticesScalar(loop, 0, skin->numVerts, vertData.data());
                }
#if defined(RC_PSP_VFPU_SKIN)
                if (vfpuSelfTest) {
                    const bool byteColours = PspSkinColoursAreBytes(skin);
                    PspVfpuSkinSelfTestVertices(vfpuTest, loop, skin->numVerts, vertData.data(),
                                                byteColours);
                    const bool passed = vfpuTest.Passed();
                    g_pspVfpuSkinMode = passed ? PspVfpuSkinMode::Enabled : PspVfpuSkinMode::Disabled;
                    PspVfpuSkinSelfTestRecord& record = g_pspVfpuSkinSelfTestRecord;
                    const f32 positionMicros = vfpuTest.maxPositionDiff * 1.0e6f;
                    record.passed = passed ? 1u : 0u;
                    record.vertices = vfpuTest.vertices;
                    record.litSources = vfpuTest.litVertices;
                    record.byteColours = byteColours ? 1u : 0u;
                    record.maxPositionDiffMicro = (positionMicros >= 0.0f && positionMicros < 4.0e9f)
                        ? static_cast<u32>(positionMicros) : 0xFFFFFFFFu;
                    record.positionFailures = vfpuTest.positionFailures;
                    record.colourDiff = vfpuTest.colourDiff;
                    record.lightDiff = vfpuTest.lightDiff;
                    record.attribMismatches = vfpuTest.attribMismatches;
                    record.magic = 0x56465055u;  // "VFPU": written last
                    LOG("[PSP VFPU skin] self-test %s: verts=%u litSources=%u byteColours=%u "
                        "maxPosDiff=%uE-6 posFailures=%u colourDiff=%u lightDiff=%u attribMismatches=%u",
                        record.passed ? "PASS (VFPU enabled)" : "FAIL (scalar kept)",
                        record.vertices, record.litSources, record.byteColours,
                        record.maxPositionDiffMicro, record.positionFailures, record.colourDiff,
                        record.lightDiff, record.attribMismatches);
                }
#endif
            }
#else
            for (u32 i = 0; i < skin->numVerts; i++) {
                const SkinVertex& sv = skin->verts[i];
                const Mat4& m = jointMatrices[sv.jointIdx];
                f32 wx, wy, wz;
#if defined(RC_PLATFORM_PSP) && defined(PSP_SKIN_TRANSFORM_CACHE)
                g_pspModelFrameScratch.transforms.Transform(
                    sv.sourceIndex, sv.jointIdx, m, sv.lx, sv.ly, sv.lz, wx, wy, wz);
#else
                Mat4TransformPoint(m, sv.lx, sv.ly, sv.lz, wx, wy, wz);
#endif

                f32 litR = sv.r;
                f32 litG = sv.g;
                f32 litB = sv.b;
                if (hasLitScratch && renderSource->primGeom && sv.sourceIndex < renderSource->primGeom->numVerts
                    && litWritten[sv.sourceIndex] != 0u) {
                    const u32 sourceIndex = static_cast<u32>(sv.sourceIndex);
                    const u16 rg = litScratch[sourceIndex * 2u + 0u];
                    const u16 b = litScratch[sourceIndex * 2u + 1u];
                    const u32 lightR = static_cast<u32>(rg & 0xFFu);
                    const u32 lightG = static_cast<u32>((rg >> 8) & 0xFFu);
                    const u32 lightB = static_cast<u32>(b & 0xFFu);

                    u32 baseR = static_cast<u32>(sv.r * 128.0f + 0.5f);
                    u32 baseG = static_cast<u32>(sv.g * 128.0f + 0.5f);
                    u32 baseB = static_cast<u32>(sv.b * 128.0f + 0.5f);
                    if (baseR > 255u) baseR = 255u;
                    if (baseG > 255u) baseG = 255u;
                    if (baseB > 255u) baseB = 255u;

                    u32 modR = (baseR * lightR + 64u) >> 7;
                    u32 modG = (baseG * lightG + 64u) >> 7;
                    u32 modB = (baseB * lightB + 64u) >> 7;
                    if (modR > 255u) modR = 255u;
                    if (modG > 255u) modG = 255u;
                    if (modB > 255u) modB = 255u;

                    litR = static_cast<f32>(modR) / 128.0f;
                    litG = static_cast<f32>(modG) / 128.0f;
                    litB = static_cast<f32>(modB) / 128.0f;
                }

                vertData[i * 10 + 0] = wx;
                vertData[i * 10 + 1] = wy;
                vertData[i * 10 + 2] = wz;
                vertData[i * 10 + 3] = litR;
                vertData[i * 10 + 4] = litG;
                vertData[i * 10 + 5] = litB;
                vertData[i * 10 + 6] = sv.u;
                vertData[i * 10 + 7] = sv.v;
                vertData[i * 10 + 8] = sv.tpage;
                vertData[i * 10 + 9] = sv.cba;
            }
#endif

            ApplyCompositeSuitFrameToVerts(renderSource ? renderSource->compositeAnim : nullptr,
                               static_cast<s32>(suitIndex),
                               skin,
                               vertData.data() + 9, 10u);

#if defined(RC_PLATFORM_PSP)
            if (!UpdatePspPrimBufferDynamicVertices(skinnedBuffer,
                                                    vertData.data(), skin->numVerts,
                                                    true))
#endif
            {
#if defined(RC_PLATFORM_PSP)
                // Keep the existing parser for an unexpected vertex format.
#endif
                skinnedBuffer->SetVertexData(vertData.data(), skin->numVerts);
            }
        }
        const bool skinBlendApplied = applySkinBlendState(skin);
        skinTimer.Stop();
        DrawTexturedMesh(skinnedBuffer
#if REAL_TEXTURE_RENDERING
                          , &renderSource->realTexGroups
#endif
                          );
        if (skinBlendApplied) {
            p3d::context->SetBlendMode(PDDI_BLEND_NONE);
        }
#if !defined(RC_PLATFORM_PSP)
        delete[] jointMatrices;
#endif
        return;
    }

    // Fallback to flat mesh
    STreeData* fallbackSkel = renderSource->skeleton;
    if (fallbackSkel && fallbackSkel->joints && fallbackSkel->joints[0].meshBuffer) {
        const bool skinBlendApplied = applySkinBlendState(renderSource->skinData);
        p3d::context->DrawPrimBuffer(fallbackSkel->joints[0].meshBuffer);
        if (skinBlendApplied) {
            p3d::context->SetBlendMode(PDDI_BLEND_NONE);
        }
    }
    else if (renderSource->meshBuffer) {
        const bool skinBlendApplied = applySkinBlendState(renderSource->skinData);
        DrawTexturedMesh(renderSource->meshBuffer
#if REAL_TEXTURE_RENDERING
                          , &renderSource->realTexGroups
#endif
                          );
        if (skinBlendApplied) {
            p3d::context->SetBlendMode(PDDI_BLEND_NONE);
        }
    }
}

// PSX: ChangeSuit__13OriginalSTreeP13DrawableSTrees (MODEL.CPP:3588)
s32 OriginalSTree::ChangeSuit(DrawableSTree* drawable, s16 suitIndexValue) {
    MARKFUNCTION(0x800722A8);

    if (!drawable || !compositeAnim) {
        return 0;
    }

    OriginalSTree* target = drawable->GetAlternateSTree();
    if (!target) {
        target = drawable->GetOriginalSTree();
    }
    RedirectCompositeSuitAnimation(compositeAnim, target ? target->primGeom : nullptr);

    // PSX calls tCompositeFlip::SetFrame(suit) then Update().
    // Host stores suit frame on drawable and applies during skinning.
    drawable->suitIndex = suitIndexValue;

    return RedirectCompositeSuitAnimation(compositeAnim, primGeom);
}

// PSX: ChangeSuit__13DrawableSTrees (MODEL.CPP:3630)
s32 DrawableSTree::ChangeSuit(s16 suitIndexValue) {
    MARKFUNCTION(0x80072350);

    if (!original) {
        return 0;
    }

    return original->ChangeSuit(this, suitIndexValue);
}

s32 DrawableSTree::MirrorTree(SModel* model) {
    MARKFUNCTION(0x8007170C);

    OriginalSTree* active = GetActiveSTree(this);
    if (!model || !active || !active->skeleton) {
        return 0;
    }

    STreeData* skeleton = active->skeleton;
    if (skeleton->jointOrderMap && skeleton->numMapEntries > 0 && !mirroredJointOrderMap) {
        mirroredJointOrderMap = new u32[skeleton->numMapEntries];
        for (u32 i = 0; i < skeleton->numMapEntries; i++) {
            mirroredJointOrderMap[i] = skeleton->jointOrderMap[i];
        }

        if (!SwapMirroredJointParamsByName(skeleton, mirroredJointOrderMap)) {
            for (u32 pairIndex = 0; kStreeMirrorSwapPairs[pairIndex] != 0; pairIndex += 2) {
                const u32 lhs = kStreeMirrorSwapPairs[pairIndex];
                const u32 rhs = kStreeMirrorSwapPairs[pairIndex + 1];
                if (lhs < skeleton->numMapEntries && rhs < skeleton->numMapEntries) {
                    const u32 temp = mirroredJointOrderMap[lhs];
                    mirroredJointOrderMap[lhs] = mirroredJointOrderMap[rhs];
                    mirroredJointOrderMap[rhs] = temp;
                }
            }
        }
    }

    mirrorFlags ^= 1u;

    AnimStructure* anim = static_cast<AnimStructure*>(model->animStructure);
    if (anim && anim->animation) {
        model->ApplyAnimToModelBasic(anim->rawAnimation ? anim->rawAnimation : anim->animation);
        if (anim->flip) {
            anim->flip->Reset();
        }
    }

    model->SetupModelCallbacks();
    return 1;
}

DrawableGeo::DrawableGeo(OriginalGeo* orig) {
    original = orig;
}

DrawableGeo::~DrawableGeo() {
    original = nullptr;
}

void DrawableGeo::Display(u32 flags) {
    if (!original || !original->meshBuffer) {
        return;
    }

#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    static u32 pspDrawableGeoProbe = 0;
    const u32 pspDrawableGeoProbeId = pspDrawableGeoProbe++;
    const bool pspDrawableProbe = pspDrawableGeoProbeId < 32u;
    if (pspDrawableProbe) {
        LOG("[PSP DrawableGeo] begin #%lu original=%p mesh=%p flags=%08lX",
            static_cast<unsigned long>(pspDrawableGeoProbeId),
            static_cast<void*>(original), static_cast<void*>(original->meshBuffer),
            static_cast<unsigned long>(flags));
    }
#endif

    DrawGeoPartMesh(original, (flags & kDisplayFlagApplyGeoLighting) != 0u);

#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    if (pspDrawableProbe) {
        LOG("[PSP DrawableGeo] done #%lu",
            static_cast<unsigned long>(pspDrawableGeoProbeId));
    }
#endif
}

DrawableETree::DrawableETree(OriginalETree* orig) {
    original = orig;
    ownerModel = nullptr;
    if (original && original->geoPartCount > 0) {
        geoPartVisible = new u8[original->geoPartCount];
        if (geoPartVisible) {
            for (u16 i = 0; i < original->geoPartCount; i++) {
                geoPartVisible[i] = 1;
            }
        }
    }
}

DrawableETree::~DrawableETree() {
    delete[] geoPartVisible;
    geoPartVisible = nullptr;
    ownerModel = nullptr;
    original = nullptr;
}

void DrawableETree::Display(u32 flags) {
    if (!original) {
        return;
    }

    if (original->geoParts && original->geoPartHashes && original->geoPartCount > 0) {
        const Mat4 baseWorld = p3d::context->GetWorldMatrix();

        STreeData* skeleton = nullptr;
        bool invokeCallbacks = false;
#if defined(RC_PLATFORM_PSP)
        std::vector<Mat4>& partJointMatrices = g_pspModelFrameScratch.partJointMatrices;
        partJointMatrices.clear();
#else
        std::vector<Mat4> partJointMatrices;
#endif
        if (ownerModel && ownerModel->animStructure && original->geoPartJointHashes) {
            AnimStructure* anim = static_cast<AnimStructure*>(ownerModel->animStructure);
            TransformFlip* flip = anim ? anim->GetFlip() : nullptr;
            if (flip && flip->tree && flip->tree->joints && flip->tree->numJoints > 0) {
                skeleton = flip->tree;
                invokeCallbacks = true;
            }
        }

        if (!skeleton && original->skeleton && original->skeleton->joints && original->skeleton->numJoints > 0) {
            skeleton = original->skeleton;
        }

        if (skeleton) {
            ResizeRetainedScratch(partJointMatrices, skeleton->numJoints);
            if (invokeCallbacks) {
                skeleton->ComputeWorldMatricesWithCallbacks(partJointMatrices.data());
            }
            else {
                skeleton->ComputeWorldMatrices(partJointMatrices.data());
            }
        }

        for (u16 i = 0; i < original->geoPartCount; i++) {
            if (geoPartVisible && geoPartVisible[i] == 0) {
                continue;
            }

            if (original->skeleton
                && original->skeleton->joints
                && i < original->skeleton->numJoints
                && ((original->skeleton->joints[i].flags & STF_HAS_MESH) == 0)) {
                continue;
            }

            OriginalGeo* geo = original->geoParts[i];
            if (!geo || !geo->meshBuffer) {
                continue;
            }

            const Mat4* partMatrix = nullptr;
            if (skeleton && partJointMatrices.data() && original->geoPartJointHashes) {
                partMatrix = FindJointWorldMatrixByHash(skeleton,
                                                        partJointMatrices.data(),
                                                        original->geoPartJointHashes[i]);
            }

            if (partMatrix) {
                Mat4 partWorld = baseWorld * (*partMatrix);
                p3d::context->SetWorldMatrix(partWorld);
            }
            else {
                p3d::context->SetWorldMatrix(baseWorld);
            }

            DrawGeoPartMesh(geo, (flags & kDisplayFlagApplyGeoLighting) != 0u);
        }

        p3d::context->SetWorldMatrix(baseWorld);
        return;
    }

    if (original->meshBuffer) {
        if (original->usesSemiTrans) {
            pddiBlendMode blendMode = PDDI_BLEND_ALPHA;
            switch (original->semiTransMode & 3u) {
                case 1: blendMode = PDDI_BLEND_ADD; break;
                case 2: blendMode = PDDI_BLEND_SUBTRACT; break;
                case 3: blendMode = PDDI_BLEND_PSX_QUARTER; break;
                default: break;
            }
            p3d::context->SetBlendMode(blendMode);
        }

        p3d::context->DrawPrimBuffer(original->meshBuffer);

        if (original->usesSemiTrans) {
            p3d::context->SetBlendMode(PDDI_BLEND_NONE);
        }
    }
}

void DrawableETree::SetGeoPartVisibleByHash(u32 hash, bool visible) {
    if (!original || !original->geoPartHashes || original->geoPartCount == 0 || !geoPartVisible) {
        return;
    }

    for (u16 i = 0; i < original->geoPartCount; i++) {
        if (original->geoPartHashes[i] == hash) {
            geoPartVisible[i] = visible ? 1 : 0;
        }
    }
}

void DrawableETree::ResetGeoPartVisibility() {
    if (!original || !geoPartVisible) {
        return;
    }

    for (u16 i = 0; i < original->geoPartCount; i++) {
        geoPartVisible[i] = 1;
    }
}

// Model
// PSX: _5Model (MODEL.CPP:671, 0x8006E654)
Model::Model() {
    MARKFUNCTION(0x8006E654);
    drawable = nullptr;
    drawableType = 0;
    animStructure = nullptr;
    field36 = nullptr;
    ambientLight = nullptr;
    hwLights = nullptr;
    hwLightCount = 0;
    rotX = 0;
    rotY = 0;
    rotZ = 0;
    posX = 0;
    posY = 0;
    posZ = 0;
    backPtr = nullptr;
    modelFlags = 0;
    Reset();
}

// PSX: __5Model (MODEL.CPP:697, 0x8006E6CC)
Model::~Model() {
    MARKFUNCTION(0x8006E6CC);

    DeleteAmbientLight();
    DeleteHardwareLights();

    if (field36) {
        delete static_cast<Shadow*>(field36);
        field36 = nullptr;
    }

    DeleteAnimStructures();
    DeleteDrawable();
}

// PSX: Reset__5Model (MODEL.CPP:777, 0x8006E86C)
void Model::Reset() {
    MARKFUNCTION(0x8006E86C);
    shadowAngle = 0;
}

void Model::Show(u32 /*flags*/) {
    // Base does nothing - overridden by SModel/GModel
}

// PSX: DrawShadow__5ModelUl (MODEL.CPP:1898, 0x80072400)
void Model::DrawShadow(u32 /*flags*/) {
    MARKFUNCTION(0x80072400);

    Shadow* shadow = GetModelShadow(this);
    if (!shadow) {
        return;
    }

    shadow->Show(nullptr);
}

void Model::Animate() {
    // Base does nothing - overridden by SModel
}

void Model::ApplyAnimToModel(s32 /*thingType*/, s32 /*animEnum*/, s32 /*p3*/, s32 /*p4*/, s32 /*p5*/) {
    // Base does nothing - overridden by SModel
}

void Model::DeleteDrawable() {
    if (drawable) {
        delete drawable;
        drawable = nullptr;
    }
    drawableType = 0;
}

// PSX: SetAnim__5Modelllil (MODEL.CPP:1907)
// Base implementation is a no-op. Overridden by HumanoidModel/PlayerModel.
void Model::SetAnim(s32 /*animEnum*/, s32 /*loopType*/, s32 /*flag*/, s32 /*extra*/) {}

// PSX: Model animation boundary handlers (MODEL.CPP:1916-2009)
// Base implementations are trampolines to AnimStructure methods.
// PSX signature: void _Handler(Model* this, AnimStructure* anim)

void Model::HandleLoop(AnimStructure* anim) { anim->Loop(); }
void Model::HandleLoopReverse(AnimStructure* anim) { anim->LoopReverse(); }
void Model::HandleHoldFirst(AnimStructure* anim) { anim->HoldFirst(); }
void Model::HandleHoldLast(AnimStructure* anim) { anim->HoldLast(); }
void Model::HandleRunToLast(AnimStructure* anim) { anim->RunToLast(); }
void Model::HandleHoldFrame(AnimStructure* /*anim*/) { /* PSX: no-op */ }
void Model::HandleRunToFrame(AnimStructure* /*anim*/) { /* PSX: no-op */ }
void Model::HandleIncFrame(AnimStructure* /*anim*/) { /* PSX: no-op */ }
void Model::HandleDecFrame(AnimStructure* anim) { anim->DecFrame(); }
void Model::HandleLoopDesired(AnimStructure* /*anim*/) { /* PSX: no-op */ }
void Model::HandleRunToLastBlend(AnimStructure* anim) { anim->RunToLastBlend(); }

// PSX: DeleteAnimStructures__5Model (MODEL.CPP:768, 0x8006E83C)
void Model::DeleteAnimStructures() {
    MARKFUNCTION(0x8006E83C);

    if (!animStructure) {
        return;
    }

    delete static_cast<AnimStructure*>(animStructure);
    animStructure = nullptr;
}

// PSX: AllocateAmbientLight__5Model (MODEL.CPP:2024, 0x8007013C)
AmbientLight* Model::AllocateAmbientLight() {
    MARKFUNCTION(0x8007013C);

    DeleteAmbientLight();
    ambientLight = new AmbientLight();
    return static_cast<AmbientLight*>(ambientLight);
}

// PSX: DeleteAmbientLight__5Model (MODEL.CPP:2030, 0x80070170)
void Model::DeleteAmbientLight() {
    MARKFUNCTION(0x80070170);

    if (!ambientLight) {
        return;
    }

    delete static_cast<AmbientLight*>(ambientLight);
    ambientLight = nullptr;
}

// PSX: AllocateHardwareLights__5ModelUl (MODEL.CPP:2039, 0x800701BC)
HardwareLight* Model::AllocateHardwareLights(u32 count) {
    MARKFUNCTION(0x800701BC);

    DeleteHardwareLights();

    if (count == 0) {
        return nullptr;
    }

    HardwareLight* lights = new HardwareLight[count]();
    hwLights = lights;
    hwLightCount = static_cast<s32>(count);
    return lights;
}

// PSX: DeleteHardwareLights__5Model (MODEL.CPP:2046, 0x80070254)
void Model::DeleteHardwareLights() {
    MARKFUNCTION(0x80070254);

    if (!hwLights) {
        hwLightCount = 0;
        return;
    }

    delete[] static_cast<HardwareLight*>(hwLights);
    hwLights = nullptr;
    hwLightCount = 0;
}

// SModel
// PSX: _6SModel (MODEL.CPP:1013, 0x8006ED68)
SModel::SModel() {
    MARKFUNCTION(0x8006ED68);
    scale = FIX16_ONE;
    field92 = 0;
}

SModel::~SModel() {
    MARKFUNCTION(0x8006EDAC);
}

// PSX: Show__6SModelUl (MODEL.CPP:1454, 0x8006F68C)
// PC adaptation: sets world matrix from position/rotation/scale, then draws mesh
void SModel::Show(u32 flags) {
    MARKFUNCTION(0x8006F68C);

    // PSX: clear visible/drawn bits
    modelFlags &= ~0x30;

    if (!drawable || !backPtr)
        return;

    if (g_environmentManager) {
        g_environmentManager->lighting.DoModelLighting(backPtr);
    }

    bool addedHwLights[3] = { false, false, false };
    if (g_environmentManager && hwLights && hwLightCount > 0) {
        HardwareLight* modelLights = static_cast<HardwareLight*>(hwLights);
        const s32 lightCount = (hwLightCount < 3) ? hwLightCount : 3;

        for (s32 slot = 0; slot < lightCount; slot++) {
            if (!modelLights[slot].active) {
                continue;
            }

            const LVector lightDir = {
                modelLights[slot].directionX,
                modelLights[slot].directionY,
                modelLights[slot].directionZ,
            };
            g_environmentManager->lighting.AddLightToPort(slot, &lightDir, modelLights[slot].colour);
            addedHwLights[slot] = true;
        }
    }

    if (ambientLight) {
        static_cast<AmbientLight*>(ambientLight)->SetPortToLight();
    }

    const u32 ownerFlags = backPtr->flags;
    const bool ownerDeadWindow = (ownerFlags & 0x80u) != 0;
    const bool ownerSemiFlag = (ownerFlags & 0x100u) != 0;
    const s32 desiredSemiMode = (ownerDeadWindow && ownerSemiFlag) ? 1 : 0;

    DrawableSTree* drawableStree = GetDrawableSTree(this);
    if (drawableStree) {
        bool semiLeakedOn = false;
        OriginalSTree* originalStree = drawableStree->GetOriginalSTree();
        OriginalSTree* alternateStree = drawableStree->GetAlternateSTree();
        if (originalStree && originalStree->skinData && originalStree->skinData->usesSemiTrans) {
            semiLeakedOn = true;
        }
        if (alternateStree && alternateStree->skinData && alternateStree->skinData->usesSemiTrans) {
            semiLeakedOn = true;
        }

        if (ownerDeadWindow || ownerSemiFlag || semiLeakedOn) {
            SetSemiModeForDrawableSTree(drawableStree, desiredSemiMode);
        }

        if (!ownerDeadWindow && ownerSemiFlag) {
            backPtr->flags &= ~0x100u;
        }
    }
    else if (!ownerDeadWindow && ownerSemiFlag) {
        backPtr->flags &= ~0x100u;
    }

    // Mark as visible + drawn
    modelFlags |= 0x50;

    // Build world matrix from position + rotation + scale
    // PSX: TransMatrix, RotMatrixZYXAndLights, ScaleMatrix (from MIPS LST)
    Mat4 world;
    p3dBuildRotMatrixZYX(rotX, rotY, rotZ, world);

    // Scale: PSX FIX16_ONE = 1.0
    f32 s = FIX16_TO_FLOAT(scale);
    world.ScaleRotation(s);

    // Translation
    world.SetTranslation((f32)posX, (f32)posY, (f32)posZ);

    const Mat4 savedWorld = p3d::context->GetWorldMatrix();
    p3d::context->SetWorldMatrix(world);

#if MODERN_GRAPHICS
    if (ShadowCSM::IsCasterPrepass()) {
        if ((backPtr->flags & 0x80u) == 0u && (modelFlags & 0x10u) != 0u && (modelFlags & 0x20u) == 0u) {
            p3d::context->SetShadowCasterInstanceId(GetShadowInstanceId(backPtr));
            ShadowCSM::DrawCasterIntoCascades(drawable, flags);
        }

        if (ambientLight) {
            RestoreWorldAmbientLightToPort();
        }
        if (g_environmentManager) {
            for (s32 slot = 0; slot < 3; slot++) {
                if (addedHwLights[slot]) {
                    g_environmentManager->lighting.RemoveLightFromPort(slot);
                }
            }
        }
        p3d::context->SetWorldMatrix(savedWorld);
        return;
    }
#endif

    // PSX: calls drawable->Display(flags) through vtable
#if MODERN_GRAPHICS
    const bool receiveMappedShadows = ShouldReceiveModelShadows(this, backPtr);
    if (receiveMappedShadows) {
        p3d::context->SetReceiveShadows(true);
        p3d::context->SetShadowReceiverInstanceId(GetShadowInstanceId(backPtr));
    }
#endif
    drawable->Display(flags);
#if MODERN_GRAPHICS
    if (receiveMappedShadows) {
        p3d::context->SetReceiveShadows(false);
        p3d::context->SetShadowReceiverInstanceId(0);
    }
#endif

    if (ambientLight) {
        RestoreWorldAmbientLightToPort();
    }

    if (g_environmentManager) {
        for (s32 slot = 0; slot < 3; slot++) {
            if (addedHwLights[slot]) {
                g_environmentManager->lighting.RemoveLightFromPort(slot);
            }
        }
    }

    p3d::context->SetWorldMatrix(savedWorld);

    if ((backPtr->flags & 0x80u) == 0u) {
        if ((modelFlags & 0x10u) != 0u && (modelFlags & 0x20u) == 0u) {
            DrawShadow(flags);
        }
    }
}

// PSX: Animate__6SModel (MODEL.CPP:1416, 0x8006F640)
void SModel::Animate() {
    MARKFUNCTION(0x8006F640);
    if (animStructure) {
        AnimStructure* anim = (AnimStructure*)animStructure;
        anim->ExecuteHandler(1);
    }
}

// PSX: InitBlendPose__6SModel (MODEL.CPP:1328, 0x8006F438)
BlendPoseState* SModel::InitBlendPose() {
    MARKFUNCTION(0x8006F438);

    AnimStructure* anim = static_cast<AnimStructure*>(animStructure);
    if (!anim || !anim->flip || !anim->flip->tree) {
        return nullptr;
    }

    const u32 jointCount = anim->flip->tree->numJoints;

    if (!anim->blendPose) {
        anim->blendPose = new BlendPoseState();
    }

    if (!anim->blendPose) {
        return nullptr;
    }

    if (anim->blendPose->jointCount != jointCount) {
        delete[] anim->blendPose->joints;
        anim->blendPose->joints = nullptr;
        anim->blendPose->jointCount = jointCount;

        if (jointCount > 0) {
            anim->blendPose->joints = new BlendJointPose[jointCount];
        }
    }

    return anim->blendPose;
}

// PSX: ApplyBlending__6SModelP10tAnimationll (MODEL.CPP:1346, 0x8006F4A0)
AnimStructure* SModel::ApplyBlending(TransformAnim* animation, s32 blendFrames, s32 startFrame) {
    MARKFUNCTION(0x8006F4A0);

    AnimStructure* anim = static_cast<AnimStructure*>(animStructure);
    if (!animation || !anim || anim->mode != 0 || !anim->flip || !anim->flip->tree) {
        return nullptr;
    }

    BlendPoseState* blendPose = InitBlendPose();
    if (!blendPose || !blendPose->joints) {
        return nullptr;
    }

    STreeData* skeleton = anim->flip->tree;
    for (u32 jointIndex = 0; jointIndex < blendPose->jointCount; jointIndex++) {
        const STreeJoint& joint = skeleton->joints[jointIndex];
        BlendJointPose& pose = blendPose->joints[jointIndex];
        pose.translationX = joint.translationX;
        pose.translationY = joint.translationY;
        pose.translationZ = joint.translationZ;
        pose.rotationX = joint.rotationX;
        pose.rotationY = joint.rotationY;
        pose.rotationZ = joint.rotationZ;
    }

    ApplyAnimToModelBasic(animation);

    anim = static_cast<AnimStructure*>(animStructure);
    if (!anim) {
        return nullptr;
    }

    anim->endFrame = blendFrames << 16;
    anim->startFrame = 0;
    anim->currentFrame = 0;
    anim->prevFrame = 0;
    anim->loopCount = 0;
    anim->SetLoopType(ANIM_BLEND2, 0);
    anim->field56 = startFrame;
    return anim;
}

// PSX: IsAnimationLoaded__6SModell (MODEL.CPP:1062, 0x8006EE4C)
s32 SModel::IsAnimationLoaded(s32 animEnum) {
    MARKFUNCTION(0x8006EE4C);

    if (!g_characterManager) {
        return 0;
    }

    const u32 thingType = backPtr ? static_cast<u32>(backPtr->thingType) : 0;
    if (g_characterManager->GetAnimation(thingType, animEnum)) {
        return 1;
    }

    return g_characterManager->GetAnimation(0, animEnum) ? 1 : 0;
}

// PSX: ApplyAnimToModel__6SModellllll (MODEL.CPP:1098, 0x8006EEAC)
void SModel::ApplyAnimToModel(s32 thingType, s32 animEnum, s32 loopType, s32 p4, s32 p5) {
    MARKFUNCTION(0x8006EEAC);
    if (!g_characterManager) {
        return;
    }

    void* rawAnimation = g_characterManager->GetAnimation((u32)thingType, animEnum);
    if (!rawAnimation) {
        rawAnimation = g_characterManager->GetAnimation(0, animEnum);
        if (!rawAnimation) {
            LOG("[ApplyAnim] missing anim type=%d animEnum=%d", thingType, animEnum);
            return;
        }
        if (thingType != 0) {
            LOG("[AnimFallback] type=%d animEnum=%d -> using PLAYER anim", thingType, animEnum);
        }
    }

    TransformAnim* animation = GetTransformAnimForModel(rawAnimation);
    if (!animation) {
        LOG("[ApplyAnimToModel] GetTransformAnimForModel NULL for type=%d animEnum=%d (CameraParamAnim or null)", thingType, animEnum);
        return;
    }

    if (!animStructure) {
        animStructure = new AnimStructure(0, rawAnimation, loopType, this, drawable);
    }

    AnimStructure* as = (AnimStructure*)animStructure;
    if (!as) {
        return;
    }

    as->animEnum = animEnum;

    if (p4 != 0) {
        as = ApplyBlending(animation, p4, p5 << 16);
        if (as && as->loopTypeField == ANIM_BLEND2) {
            as->animEnum = animEnum;
            if (as->blendPose) {
                as->blendPose->loopType = loopType;
            }
            as->humanoidCB = {};
            return;
        }
    }

    ApplyAnimToModelBasic(rawAnimation);

    as = (AnimStructure*)animStructure;
    if (!as) {
        return;
    }

    as->animEnum = animEnum;
    if (as->flip) {
        as->flip->Reset();
    }
    as->SetLoopType(loopType, 1);
    as->humanoidCB = {};
}

void SModel::ApplyAnimToModelBasic(void* rawAnimation) {
    MARKFUNCTION(0x8006F068);

    TransformAnim* animation = GetTransformAnimForModel(rawAnimation);
    if (!animation) {
        return;
    }

    AnimStructure* anim = static_cast<AnimStructure*>(animStructure);
    if (!anim || anim->mode != 0 || !anim->flip) {
        delete anim;
        anim = new AnimStructure(0, rawAnimation, 0, this, drawable);
        animStructure = anim;
    }

    if (!anim || !anim->flip) {
        return;
    }

    // Sequence: update sequence pointer and keep animation = part 0.
    if (IsCharSequenceAnim(rawAnimation)) {
        anim->rawAnimation = rawAnimation;
        anim->sequence = static_cast<CharSequenceAnim*>(rawAnimation);
        anim->animation = animation;
    }
    else {
        anim->rawAnimation = rawAnimation;
        anim->sequence = nullptr;
        anim->animation = animation;
    }

    SyncFlipTreeWithDrawable(this, anim);
    anim->flip->anim = animation;
    UpdateFlipMirrorState(this, anim);
    anim->flip->dirty = 1;

    anim->startFrame = 0;
    anim->currentFrame = 0;
    const s32 totalFrames = anim->sequence ? anim->sequence->numFrames : animation->numFrames;
    anim->endFrame = (totalFrames > 0) ? ((totalFrames - 1) << 16) : 0;
    anim->prevFrame = 0;
    anim->loopCount = 0;
    anim->speed = FIX16_ONE;

}

// PSX: SetOriginalSTree__6SModelP13OriginalSTreeP10tAnimation (MODEL.CPP:1026, 0x8006EDD4)
void SModel::SetOriginalSTree(OriginalSTree* original) {
    MARKFUNCTION(0x8006EDD4);
    DeleteDrawable();
    drawable = new DrawableSTree(original);
    drawableType = 2; // STree type
    SetupModelCallbacks();
}

// PSX: SetupModelCallbacks__6SModel (MODEL.HPP:1097, 0x80072440)
void SModel::SetupModelCallbacks() {
    MARKFUNCTION(0x80072440);
}

s32 SModel::MirrorTree() {
    MARKFUNCTION(0x8006FAD4);

    DrawableSTree* drawableStree = GetDrawableSTree(this);
    if (!drawableStree) {
        return 0;
    }

    return drawableStree->MirrorTree(this);
}

// PSX: InitSemiTransMode__6SModel (MODEL.CPP:1045, 0x8006EE20)
void SModel::InitSemiTransMode() {
    MARKFUNCTION(0x8006EE20);

    DrawableSTree* drawableStree = GetDrawableSTree(this);
    if (!drawableStree) {
        return;
    }

    SetSemiModeForDrawableSTree(drawableStree, 0);
}

// PSX: PlayDynamicAnim__6SModeli (MODEL.CPP:1710, 0x8006FAFC)
void SModel::PlayDynamicAnim(s32 animEnumVal) {
    MARKFUNCTION(0x8006FAFC);
    SetAnim(animEnumVal, 0, 0, 0);
    AnimStructure* anim = static_cast<AnimStructure*>(animStructure);
    if (anim) {
        anim->animEnum = animEnumVal;
    }
}

GModel::GModel() {
    MARKFUNCTION(0x8006E8C8);

    std::memset(attachedMatrix, 0, sizeof(attachedMatrix));
    s16* rot = reinterpret_cast<s16*>(attachedMatrix);
    rot[0] = 0x1000;
    rot[4] = 0x1000;
    rot[8] = 0x1000;
    attachedMatrixActive = 0;
}

GModel::~GModel() {
    MARKFUNCTION(0x8006E908);
}

void GModel::Show(u32 flags) {
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    static u32 pspGModelEntryProbe = 0;
    const u32 pspGModelEntryProbeId = pspGModelEntryProbe++;
    if (pspGModelEntryProbeId < 16u) {
        LOG("[PSP GModel] entry #%lu model=%p vword=%08lX drawable=%p back=%p flags=%08lX",
            static_cast<unsigned long>(pspGModelEntryProbeId), static_cast<void*>(this),
            static_cast<unsigned long>(reinterpret_cast<const u32*>(this)[0]),
            static_cast<void*>(drawable), static_cast<void*>(backPtr),
            static_cast<unsigned long>(flags));
    }
#endif
    MARKFUNCTION(0x8006EA7C);

    modelFlags &= ~0x30;

    if (!drawable || !backPtr) {
        return;
    }

#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    static u32 pspGModelProbe = 0;
    const u32 pspGModelProbeId = pspGModelProbe++;
    const bool pspProbe = backPtr->thingType == 201u && pspGModelProbeId < 12u;
    if (pspProbe) {
        LOG("[PSP GModel] begin #%lu model=%p drawable=%p ambient=%p hw=%p count=%ld flags=%08lX",
            static_cast<unsigned long>(pspGModelProbeId), static_cast<void*>(this),
            static_cast<void*>(drawable), static_cast<void*>(ambientLight), hwLights,
            static_cast<long>(hwLightCount), static_cast<unsigned long>(flags));
    }
#else
    const bool pspProbe = false;
#endif

#if defined(RC_PLATFORM_PSP)
    const bool pspSkipLocalLights = backPtr->thingType == 201u;
#else
    const bool pspSkipLocalLights = false;
#endif

    // Most PSP geometry models (platforms, props and collision scenery) do
    // not own a local ambient/hardware light. Avoid walking the full lighting
    // volume graph for those objects: besides being wasted work, the legacy
    // linked-list lighting data can be transient while a level is streaming.
    // Models that actually carry light state still use the original path.
#if defined(RC_PLATFORM_PSP)
    const bool needsModelLighting = false;
#else
    const bool needsModelLighting = true;
#endif
    if (g_environmentManager && needsModelLighting && !pspSkipLocalLights) {
        const u16 thingType = backPtr->thingType;
        const bool inGatedTypeRange = (thingType >= 400u && thingType < 474u);
        const bool forceLighting = (*(reinterpret_cast<const u8*>(backPtr) + 113u) != 0u);
        if (!inGatedTypeRange || forceLighting) {
            g_environmentManager->lighting.DoModelLighting(backPtr);
        }
    }
    if (pspProbe) {
        LOG("[PSP GModel] after lighting");
    }

    bool addedHwLights[3] = { false, false, false };
    if (g_environmentManager && !pspSkipLocalLights && hwLights && hwLightCount > 0) {
        HardwareLight* modelLights = static_cast<HardwareLight*>(hwLights);
        const s32 lightCount = (hwLightCount < 3) ? hwLightCount : 3;

        for (s32 slot = 0; slot < lightCount; slot++) {
            if (!modelLights[slot].active) {
                continue;
            }

            const LVector lightDir = {
                modelLights[slot].directionX,
                modelLights[slot].directionY,
                modelLights[slot].directionZ,
            };
            g_environmentManager->lighting.AddLightToPort(slot, &lightDir, modelLights[slot].colour);
            addedHwLights[slot] = true;
        }
    }

    if (!pspSkipLocalLights && ambientLight) {
        static_cast<AmbientLight*>(ambientLight)->SetPortToLight();
    }

    if (pspProbe) {
        LOG("[PSP GModel] after lights ambient=%p hw=%p count=%ld",
            static_cast<void*>(ambientLight), hwLights, static_cast<long>(hwLightCount));
    }

    modelFlags |= 0x50;

    const LVector modelPos = { posX, posY, posZ };
    Mat4 billboardMatrix;
    bool hasBillboardMatrix = false;
    if ((modelFlags & 2u) != 0u) {
        MakeBillboardMatrix(modelPos, billboardMatrix, 1);
        hasBillboardMatrix = true;
    }
    if ((modelFlags & 4u) != 0u) {
        MakeBillboardMatrixFlip(modelPos, billboardMatrix, 0);
        hasBillboardMatrix = true;
    }

    Mat4 world;
    if ((modelFlags & 1u) != 0u) {
        if (hasBillboardMatrix) {
            world = billboardMatrix;
        }
        else if (attachedMatrixActive != 0) {
            BuildMat4FromPsxPackedMatrix(attachedMatrix, world);
        }
        else {
            p3dBuildRotMatrixZYX(rotX, rotY, rotZ, world);
        }
    }
    else {
        p3dBuildRotMatrixZYX(rotX, rotY, rotZ, world);
    }
    world.SetTranslation((f32)posX, (f32)posY, (f32)posZ);

    const Mat4 savedWorld = p3d::context->GetWorldMatrix();
    p3d::context->SetWorldMatrix(world);

    if (pspProbe) {
        LOG("[PSP GModel] after world matrix");
    }

    const bool applyGeoLighting = !pspSkipLocalLights && (ambientLight || (hwLights && hwLightCount > 0));
    const u32 displayFlags = applyGeoLighting ? (flags | kDisplayFlagApplyGeoLighting) : flags;

#if MODERN_GRAPHICS
    if (ShadowCSM::IsCasterPrepass()) {
        if ((modelFlags & 0x10u) != 0u && (modelFlags & 0x20u) == 0u) {
            p3d::context->SetShadowCasterInstanceId(GetShadowInstanceId(backPtr));
            ShadowCSM::DrawCasterIntoCascades(drawable, displayFlags);
        }

        if (!pspSkipLocalLights && ambientLight) {
            RestoreWorldAmbientLightToPort();
        }
        if (g_environmentManager) {
            for (s32 slot = 0; slot < 3; slot++) {
                if (addedHwLights[slot]) {
                    g_environmentManager->lighting.RemoveLightFromPort(slot);
                }
            }
        }
        p3d::context->SetWorldMatrix(savedWorld);
        return;
    }
#endif

#if MODERN_GRAPHICS
    const bool receiveMappedShadows = ShouldReceiveModelShadows(this, backPtr);
    if (receiveMappedShadows) {
        p3d::context->SetReceiveShadows(true);
        p3d::context->SetShadowReceiverInstanceId(GetShadowInstanceId(backPtr));
    }
#endif
    drawable->Display(displayFlags);
    if (pspProbe) {
        LOG("[PSP GModel] after drawable display");
    }
#if MODERN_GRAPHICS
    if (receiveMappedShadows) {
        p3d::context->SetReceiveShadows(false);
        p3d::context->SetShadowReceiverInstanceId(0);
    }
#endif

    if (!pspSkipLocalLights && ambientLight) {
        RestoreWorldAmbientLightToPort();
    }

    if (g_environmentManager && !pspSkipLocalLights) {
        for (s32 slot = 0; slot < 3; slot++) {
            if (addedHwLights[slot]) {
                g_environmentManager->lighting.RemoveLightFromPort(slot);
            }
        }
    }

    p3d::context->SetWorldMatrix(savedWorld);

    if ((modelFlags & 0x10u) != 0u && (modelFlags & 0x20u) == 0u) {
        DrawShadow(flags);
    }
}

void GModel::SetOriginalGeo(OriginalGeo* original) {
    DeleteDrawable();
    drawable = new DrawableGeo(original);
    drawableType = 1;
}

EModel::EModel() {
    MARKFUNCTION(0x8006FB50);
}

EModel::~EModel() {
    MARKFUNCTION(0x8006FB84);
}

void EModel::SetOriginalETree(OriginalETree* original, TransformAnim* animation) {
    MARKFUNCTION(0x8006FBAC);
    DeleteDrawable();
    DrawableETree* drawableETree = new DrawableETree(original);
    drawableETree->SetOwnerModel(this);
    drawable = drawableETree;
    drawableType = 3;

    if (animation) {
        if (animStructure) {
            delete static_cast<AnimStructure*>(animStructure);
            animStructure = nullptr;
        }
        animStructure = new AnimStructure(1, animation, ANIM_LOOP, this, drawable);
    }
}

void EModel::ApplyAnimToModel(s32 thingType, s32 animEnum, s32 loopType, s32 /*p4*/, s32 /*p5*/) {
    MARKFUNCTION(0x8006FC34);
    if (!g_characterManager) {
        return;
    }

    TransformAnim* anim = (TransformAnim*)g_characterManager->GetAnimation((u32)thingType, animEnum);
    if (!anim) {
        anim = (TransformAnim*)g_characterManager->GetAnimation(0, animEnum);
        if (!anim) {
            return;
        }
    }

    if (animStructure) {
        delete static_cast<AnimStructure*>(animStructure);
        animStructure = nullptr;
    }

    ApplyAnimToModel(anim, loopType);
}

AnimStructure* EModel::ApplyAnimToModel(TransformAnim* animation, s32 loopType) {
    MARKFUNCTION(0x8006FCAC);

    AnimStructure* animStruct = new AnimStructure(2, animation, loopType, this, drawable);
    animStructure = animStruct;
    return animStruct;
}

void EModel::Animate() {
    MARKFUNCTION(0x8006FD10);
    if (!animStructure) {
        return;
    }

    s32 doFlip = ((modelFlags >> 6) & 1) ? 1 : 0;
    static_cast<AnimStructure*>(animStructure)->ExecuteHandler(doFlip);
}

void EModel::Show(u32 flags) {
    MARKFUNCTION(0x8006FD44);

    modelFlags &= ~0x30;

    if (!drawable || !backPtr) {
        return;
    }

    if (g_environmentManager) {
        g_environmentManager->lighting.DoModelLighting(backPtr);
    }

    bool addedHwLights[3] = { false, false, false };
    if (g_environmentManager && hwLights && hwLightCount > 0) {
        HardwareLight* modelLights = static_cast<HardwareLight*>(hwLights);
        const s32 lightCount = (hwLightCount < 3) ? hwLightCount : 3;

        for (s32 slot = 0; slot < lightCount; slot++) {
            if (!modelLights[slot].active) {
                continue;
            }

            const LVector lightDir = {
                modelLights[slot].directionX,
                modelLights[slot].directionY,
                modelLights[slot].directionZ,
            };
            g_environmentManager->lighting.AddLightToPort(slot, &lightDir, modelLights[slot].colour);
            addedHwLights[slot] = true;
        }
    }

    if (ambientLight) {
        static_cast<AmbientLight*>(ambientLight)->SetPortToLight();
    }

    modelFlags |= 0x50;

    Mat4 world;
    p3dBuildRotMatrixZYX(rotX, rotY, rotZ, world);
    world.SetTranslation((f32)posX, (f32)posY, (f32)posZ);

    const Mat4 savedWorld = p3d::context->GetWorldMatrix();
    p3d::context->SetWorldMatrix(world);

    const bool applyGeoLighting = ambientLight || (hwLights && hwLightCount > 0);
    const u32 displayFlags = applyGeoLighting ? (flags | kDisplayFlagApplyGeoLighting) : flags;

#if MODERN_GRAPHICS
    if (ShadowCSM::IsCasterPrepass()) {
        if ((modelFlags & 0x10u) != 0u && (modelFlags & 0x20u) == 0u) {
            p3d::context->SetShadowCasterInstanceId(GetShadowInstanceId(backPtr));
            ShadowCSM::DrawCasterIntoCascades(drawable, displayFlags);
        }

        if (ambientLight) {
            RestoreWorldAmbientLightToPort();
        }
        if (g_environmentManager) {
            for (s32 slot = 0; slot < 3; slot++) {
                if (addedHwLights[slot]) {
                    g_environmentManager->lighting.RemoveLightFromPort(slot);
                }
            }
        }
        p3d::context->SetWorldMatrix(savedWorld);
        return;
    }
#endif

#if MODERN_GRAPHICS
    const bool receiveMappedShadows = ShouldReceiveModelShadows(this, backPtr);
    if (receiveMappedShadows) {
        p3d::context->SetReceiveShadows(true);
        p3d::context->SetShadowReceiverInstanceId(GetShadowInstanceId(backPtr));
    }
#endif
    drawable->Display(displayFlags);
#if MODERN_GRAPHICS
    if (receiveMappedShadows) {
        p3d::context->SetReceiveShadows(false);
        p3d::context->SetShadowReceiverInstanceId(0);
    }
#endif

    if (ambientLight) {
        RestoreWorldAmbientLightToPort();
    }

    if (g_environmentManager) {
        for (s32 slot = 0; slot < 3; slot++) {
            if (addedHwLights[slot]) {
                g_environmentManager->lighting.RemoveLightFromPort(slot);
            }
        }
    }

    p3d::context->SetWorldMatrix(savedWorld);
}

// HumanoidModel

// PSX: SetAnim__13HumanoidModelllil (MHUMAN.CPP:119, 0x8006E1B0)
// Routes anims to ApplyAnimToModel. Transition anims (37-38) get
// special blend-from-current-frame handling. High enum anims default to
// RunToLast unless explicitly routed to Loop/HoldFirst by the PSX tree.
void HumanoidModel::SetAnim(s32 animEnum, s32 a3, s32 force, s32 extra) {
    MARKFUNCTION(0x8006E1B0);

    AnimStructure* as = (AnimStructure*)animStructure;
    // PSX: early-exit if not forcing and anim already matches
    if (!force && as && as->animEnum == animEnum) {
        return;
    }

    // A non-forced request to (re)play a looping animation must not cut a
    // one-shot animation (an attack, jump, ...) that is still playing. The
    // fighting AI re-applies its stance loop every frame; without this guard the
    // attack animation is only visible for a single frame, so enemies appear to
    // stay stiff in their fighting pose while attacking.
    if (!force && as && as->loopTypeField == ANIM_RUN_TO_LAST && as->loopCount == 0
        && as->endFrame > 0 && as->currentFrame < as->endFrame) {
        const bool incomingLoop = (animEnum == 1 || animEnum == 2 || animEnum == 4 || animEnum == 15
            || animEnum == 22 || animEnum == 43 || animEnum == 45 || animEnum == 49 || animEnum == 50
            || animEnum == 314 || animEnum == 315);
        if (incomingLoop) {
            return;
        }
    }

    if (backPtr && backPtr->thingType != 0) {
        LOG("[SetAnim] id=%p type=%u anim=%d a3=%d force=%d extra=%d",
            static_cast<void*>(backPtr), static_cast<u32>(backPtr->thingType), animEnum, a3, force, extra);
    }

    // PSX: reads thingType from backPtr + 24 (Thing::thingType)
    s32 thingType = backPtr->thingType;

    if (animEnum == 0) {
        ApplyAnimToModel(thingType, animEnum, ANIM_HOLD_FIRST, a3, extra);
        return;
    }

    if (animEnum == 1 || animEnum == 2 || animEnum == 4 || animEnum == 15 || animEnum == 22
        || animEnum == 43 || animEnum == 45 || animEnum == 49 || animEnum == 50
        || animEnum == 314 || animEnum == 315) {
        ApplyAnimToModel(thingType, animEnum, ANIM_LOOP, a3, extra);
        return;
    }

    // PSX: anims 37-38 are transition anims with blend from current frame
    if (animEnum >= 37 && animEnum <= 38) {
        // PSX: ApplyAnimToModel using current frame as start, loopType=2
        ApplyAnimToModel(thingType, animEnum, 2, a3, extra);
        // PSX: sets humanoidCB blend data {4063232, 8}
        as = (AnimStructure*)animStructure;
        if (as) {
            as->humanoidCB.offsetLo = (s16)(4063232 & 0xFFFF);
            as->humanoidCB.offsetHi = (s16)(4063232 >> 16);
            as->humanoidCB.funcPtr = reinterpret_cast<void*>(static_cast<intptr_t>(8));
        }
        return;
    }

    // PSX default path uses RunToLast for all remaining humanoid anims.
    ApplyAnimToModel(thingType, animEnum, ANIM_RUN_TO_LAST, a3, extra);
}

// PSX: _13HumanoidModel (MHUMAN.CPP:45, 0x8006E020)
HumanoidModel::HumanoidModel() {
    MARKFUNCTION(0x8006E020);
    AllocateHardwareLights(3);
    AllocateAmbientLight();

    // PSX allocates AnimationMatrices (660 bytes on PSX layout).
    animMatrices = new AnimationMatrices();
    attackHandRadius = 100;
    attackFootRadius = 100;
    field108 = 100;
    field112 = 400;
    field116 = 100;
    field120 = 0;
    headTrackDir = { 0, 0, 0xFFFF };
}

// PSX: __13HumanoidModel (MHUMAN.CPP:56, 0x8006E0C8)
HumanoidModel::~HumanoidModel() {
    MARKFUNCTION(0x8006E0C8);
    if (animMatrices) {
        delete animMatrices;
        animMatrices = nullptr;
    }
}

// PSX: Animate__13HumanoidModel (MHUMAN.CPP:207, 0x8006E418)
static u32 sDbgAnimTick = 0;

void HumanoidModel::Animate() {
    MARKFUNCTION(0x8006E418);
    ++sDbgAnimTick;
    SModel::Animate();

    Humanoid* owner = static_cast<Humanoid*>(backPtr);
    if (owner && owner->thingType != 0 && animStructure) {
        AnimStructure* dbgAnim = static_cast<AnimStructure*>(animStructure);
        if (dbgAnim->loopTypeField == ANIM_RUN_TO_LAST) {
            u32 poseHash = 0;
            OriginalSTree* dbgActive = GetActiveSTree(drawable);
            STreeData* dbgSkel = dbgActive ? dbgActive->skeleton : nullptr;
            if (dbgSkel && dbgSkel->joints) {
                for (u32 ji = 0; ji < dbgSkel->numJoints; ji++) {
                    poseHash = poseHash * 31u + static_cast<u32>(static_cast<u16>(dbgSkel->joints[ji].rotationX));
                    poseHash = poseHash * 31u + static_cast<u32>(static_cast<u16>(dbgSkel->joints[ji].rotationY));
                    poseHash = poseHash * 31u + static_cast<u32>(static_cast<u16>(dbgSkel->joints[ji].rotationZ));
                }
            }
            LOG("[Act] id=%p drw=%p t=%u type=%u anim=%d frame=%d end=%d lc=%d pose=%08X",
                static_cast<void*>(owner), static_cast<void*>(drawable), sDbgAnimTick,
                static_cast<u32>(owner->thingType),
                dbgAnim->animEnum, static_cast<s32>(dbgAnim->currentFrame >> 16),
                static_cast<s32>(dbgAnim->endFrame >> 16), dbgAnim->loopCount, poseHash);
        }
    }
    if (owner && owner->humanoidSound && animStructure) {
        AnimStructure* anim = static_cast<AnimStructure*>(animStructure);
        owner->humanoidSound->ProcessSoundScript(
            (u32)anim->animEnum,
            (u32)(s16)((u32)anim->currentFrame >> 16)
        );
    }
}

void HumanoidModel::CaptureAttackJointMatrices() {
    if (!animMatrices || !drawable) {
        return;
    }

    OriginalSTree* active = GetActiveSTree(drawable);
    if (!active || !active->skeleton) {
        return;
    }

    STreeData* skel = active->skeleton;
    if (!skel->joints || skel->numJoints == 0) {
        return;
    }

    // Force capture on regardless of the render-side gate: this evaluation is the
    // authoritative gameplay snapshot. CopyMatrix builds each joint's world matrix
    // from the owning Humanoid's authoritative pos/orientation, so the captured
    // attack-joint positions are independent of the render context world matrix.
    const s32 savedCapture = animMatrices->CaptureEnabled();
    animMatrices->SetCaptureEnabled(1);

#if defined(RC_PLATFORM_PSP)
    std::vector<Mat4>& jointWorld = g_pspModelFrameScratch.attackJointMatrices;
    ResizeRetainedScratch(jointWorld, skel->numJoints);
    skel->ComputeWorldMatricesWithCallbacks(jointWorld.data());
#else
    std::vector<Mat4> jointWorld(skel->numJoints);
    skel->ComputeWorldMatricesWithCallbacks(jointWorld.data());
#endif

    animMatrices->SetCaptureEnabled(savedCapture);
}

// PSX: SetupModelCallbacks__13HumanoidModel (MHUMAN.CPP:69, 0x8006E114)
void HumanoidModel::SetupModelCallbacks() {
    MARKFUNCTION(0x8006E114);

    Shadow* shadow = GetModelShadow(this);
    if (!shadow || shadow->GetFloorHeightState()->shadowType != MODEL_SHADOW_TREE) {
        delete shadow;
        field36 = new TreeShadow(this);
    }

    if (!animMatrices) {
        return;
    }

    animMatrices->SetupCallbacks(this);
    animMatrices->SetHumanoid(backPtr);
    if (backPtr == Player::s_player) {
        animMatrices->SetupExtraCallbacks(this);
    }
}

// PSX: _Loop__13HumanoidModelP13AnimStructure (MHUMAN.CPP:196)
// Only calls base Loop when mode == 0 (normal). For reverse/runToLast/camera
// modes, the loop handler intentionally does nothing.
void HumanoidModel::HandleLoop(AnimStructure* anim) {
    if (anim->mode == 0) {
        Model::HandleLoop(anim);
    }
}
