#include "gen/model.h"
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
#ifdef REAL_TEXTURE_RENDERING
#include "extra/realtexture.h"
#endif
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

#include "extra/shadowcsm.h"

// Draws a mesh buffer, using the real-texture rendering path (one bound real
// 2D texture per material, sub-range draws) when it's enabled and the mesh
// has grouped index ranges; otherwise falls back to the single legacy draw
// that samples the shared PSX VRAM atlas, unchanged from before this feature.
static void DrawTexturedMesh(pddiPrimBuffer* buffer
#ifdef REAL_TEXTURE_RENDERING
                              , const std::vector<RealTextureGroup>* groups
#endif
                              ) {
    if (!buffer) return;

#ifdef REAL_TEXTURE_RENDERING
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

static void ApplyClutListSuitFrameToVerts(const ClutAnimData* clutAnim,
                                          s32 frame,
                                          const SkinData* skin,
                                          std::vector<f32>* vertData) {
    if (!clutAnim || !skin || !vertData || !clutAnim->frames || clutAnim->numFrames <= 0) {
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
            for (u32 primIndex = 0; primIndex < skin->numPrims; primIndex++) {
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

                    (*vertData)[vertexIndex * 10u + 9u] = clutWord;
                }

                matchedOffsets++;
                break;
            }
        }

        return;
    }

    for (u32 vertexIndex = 0; vertexIndex < skin->numVerts; vertexIndex++) {
        (*vertData)[vertexIndex * 10u + 9u] = clutWord;
    }
}

static void ApplyCompositeSuitFrameToVerts(CompositeAnimData* compositeAnim,
                                           s32 frame,
                                           const SkinData* skin,
                                           std::vector<f32>* vertData) {
    compositeAnim = ResolveCompositeAnimRootForSuit(compositeAnim);

    if (!compositeAnim || !compositeAnim->parts || !skin || !vertData) {
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
            ApplyCompositeSuitFrameToVerts(part.compositeAnim, partFrame, skin, vertData);
        }
        else if (entityType == kSuitEntityTypeClutList && part.clutAnim) {
            resolvedClutParts++;
            const s32 partFrame = ResolveSuitPartFrame(&part, frame, part.clutAnim->numFrames);
            ApplyClutListSuitFrameToVerts(part.clutAnim, partFrame, skin, vertData);
        }
        else {
            unresolvedParts++;
        }
    }
}


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

static bool BuildLitGeoVerts(OriginalGeo* geo, std::vector<GeoRenderVertex>& outVerts) {
    if (!geo || !geo->dynamicVerts || geo->dynamicVertCount == 0 || !geo->dynamicVertSourceIndex
        || !geo->primGeom || !geo->primGeom->GetVertexList()) {
        return false;
    }

    const u8* sourceVerts = geo->primGeom->GetVertexList();
    outVerts.assign(geo->dynamicVerts, geo->dynamicVerts + geo->dynamicVertCount);

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

    for (u32 i = 0; i < geo->dynamicVertCount; i++) {
        const u32 sourceIndex = static_cast<u32>(geo->dynamicVertSourceIndex[i]);
        if (sourceIndex >= geo->primGeom->numVerts) {
            continue;
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

        u32 baseR = static_cast<u32>(outVerts[i].r * 128.0f + 0.5f);
        u32 baseG = static_cast<u32>(outVerts[i].g * 128.0f + 0.5f);
        u32 baseB = static_cast<u32>(outVerts[i].b * 128.0f + 0.5f);
        if (baseR > 255u) baseR = 255u;
        if (baseG > 255u) baseG = 255u;
        if (baseB > 255u) baseB = 255u;

        outVerts[i].r = static_cast<f32>(ModulatePsxColourChannel(static_cast<u8>(baseR), litI)) / 128.0f;
        outVerts[i].g = static_cast<f32>(ModulatePsxColourChannel(static_cast<u8>(baseG), litI)) / 128.0f;
        outVerts[i].b = static_cast<f32>(ModulatePsxColourChannel(static_cast<u8>(baseB), litI)) / 128.0f;
    }

    return true;
}

static void DrawGeoPartMesh(OriginalGeo* geo, bool applyLighting) {
    if (!geo || !geo->meshBuffer) {
        return;
    }

    p3d::context->SetCullMode(PDDI_CULL_NONE);

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

    // Reused across frames; BuildLitGeoVerts assign()s over it, so only its
    // capacity carries between calls.
    static std::vector<GeoRenderVertex> litVerts;
    const bool litVertsApplied = applyLighting && BuildLitGeoVerts(geo, litVerts);
    if (litVertsApplied) {
        geo->meshBuffer->SetVertexData(litVerts.data(), geo->dynamicVertCount);
    }

    DrawTexturedMesh(geo->meshBuffer
#ifdef REAL_TEXTURE_RENDERING
                      , &geo->realTexGroups
#endif
                      );

    if (litVertsApplied) {
        geo->meshBuffer->SetVertexData(geo->dynamicVerts, geo->dynamicVertCount);
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
        // Reused across frames and characters: this ran a heap allocation per
        // character per frame, on the render path. Safe as a shared buffer
        // because the joint pre/post callbacks ComputeWorldMatricesWithCallbacks
        // fires only adjust joint transforms -- none of them re-enter drawing,
        // so nothing can resize this out from under the pointer below.
        static std::vector<Mat4> jointMatrixScratch;
        if (jointMatrixScratch.size() < skel->numJoints) {
            jointMatrixScratch.resize(skel->numJoints);
        }
        Mat4* jointMatrices = jointMatrixScratch.data();
        skel->ComputeWorldMatricesWithCallbacks(jointMatrices);
        const Mat4 modelWorld = p3d::context->GetWorldMatrix();
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

        std::vector<u16> litScratch;
        std::vector<u8> litWritten;
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
        if (renderSource->xformVertsCallback && renderSource->primGeom && renderSource->primGeom->numVerts > 0) {
            const u32 sourceVertCount = renderSource->primGeom->numVerts;
            const u8* sourceVertexList = renderSource->primGeom->GetVertexList();
            litScratch.resize(sourceVertCount * 2u, 0u);
            litWritten.resize(sourceVertCount, 0u);

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
                Mat4 lightingMatrix = lightingWorld * jointMatrices[jointIndex];
                renderSource->xformVertsCallback(renderSource->primGeom,
                                                 &clampedJoint,
                                                 reinterpret_cast<u32*>(&lightingMatrix),
                                                 litScratch.data());
            }

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

        // Reused across frames: allocating and zeroing numVerts*10 floats every
        // frame per character was pure churn, since the loop below overwrites
        // all of it. Consumers index by vertex, never by vertData.size(), so a
        // buffer left larger than needed is harmless.
        static std::vector<f32> vertData;
        if (vertData.size() < static_cast<size_t>(skin->numVerts) * 10u) {
            vertData.resize(static_cast<size_t>(skin->numVerts) * 10u);
        }
        for (u32 i = 0; i < skin->numVerts; i++) {
            const SkinVertex& sv = skin->verts[i];
            const Mat4& m = jointMatrices[sv.jointIdx];
            f32 wx, wy, wz;
            Mat4TransformPoint(m, sv.lx, sv.ly, sv.lz, wx, wy, wz);

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

        ApplyCompositeSuitFrameToVerts(renderSource ? renderSource->compositeAnim : nullptr,
                           static_cast<s32>(suitIndex),
                           skin,
                           &vertData);

        skinnedBuffer->SetVertexData(vertData.data(), skin->numVerts);
        const bool skinBlendApplied = applySkinBlendState(skin);
        DrawTexturedMesh(skinnedBuffer
#ifdef REAL_TEXTURE_RENDERING
                          , &renderSource->realTexGroups
#endif
                          );
        if (skinBlendApplied) {
            p3d::context->SetBlendMode(PDDI_BLEND_NONE);
        }
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
#ifdef REAL_TEXTURE_RENDERING
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

    DrawGeoPartMesh(original, (flags & kDisplayFlagApplyGeoLighting) != 0u);
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
        std::vector<Mat4> partJointMatrices;
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
            partJointMatrices.resize(skeleton->numJoints);
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
    MARKFUNCTION(0x8006EA7C);

    modelFlags &= ~0x30;

    if (!drawable || !backPtr) {
        return;
    }

    if (g_environmentManager) {
        const u16 thingType = backPtr->thingType;
        const bool inGatedTypeRange = (thingType >= 400u && thingType < 474u);
        const bool forceLighting = (*(reinterpret_cast<const u8*>(backPtr) + 113u) != 0u);
        if (!inGatedTypeRange || forceLighting) {
            g_environmentManager->lighting.DoModelLighting(backPtr);
        }
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
void HumanoidModel::Animate() {
    MARKFUNCTION(0x8006E418);
    SModel::Animate();

    Humanoid* owner = static_cast<Humanoid*>(backPtr);
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

    std::vector<Mat4> jointWorld(skel->numJoints);
    skel->ComputeWorldMatricesWithCallbacks(jointWorld.data());

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
