#include "ai/collect.h"

#include "ai/humanoid.h"
#include "ai/obstacle_shared.h"
#include "ai/player.h"
#include "fe/hud.h"
#include "gen/animmgr.h"
#include "gen/animstruct.h"
#include "gen/colvol.h"
#include "gen/database.h"
#include "gen/game.h"
#include "gen/model.h"
#include "gen/time.h"
#include "gen/scoremgr.h"
#include "snd/snddrct.h"

static constexpr s32 COLLECT_FLOAT_STEP = 1092;
static constexpr s32 COLLECT_PICKUP_PAD_XZ = 96;  // PC: extra grab margin on each XZ side
static constexpr s32 COLLECT_PICKUP_PAD_Y = 64;   // PC: extra grab margin on each Y side

static bool CollectibleBoxLooksValid(const tagCollisionBox& box) {
    return box.maxX >= box.minX && box.maxY >= box.minY && box.maxZ >= box.minZ;
}
static constexpr s32 COLLECT_MISC_ANIM_MAX_DEPTH = 16;
static constexpr u32 COLLECT_ENTITY_TYPE_COMPOSITE_ANIM = 0x00010001u;
static constexpr u32 COLLECT_ENTITY_TYPE_TREE_FLIP = 0x00020003u;

static u32 Collectible_GetMiscEntityType(const MiscAnimNode* node) {
    if (!node) {
        return 0;
    }

    if (node->compositeAnim) {
        return COLLECT_ENTITY_TYPE_COMPOSITE_ANIM;
    }

    if (node->anim) {
        return COLLECT_ENTITY_TYPE_TREE_FLIP;
    }

    return 0;
}

static s32 Collectible_GetMiscAnimFrameCount(MiscAnimNode* node) {
    if (!node) {
        return 0;
    }

    if (node->anim) {
        return node->anim->numFrames;
    }

    if (node->sequenceAnim) {
        return node->sequenceAnim->numFrames;
    }

    if (node->compositeAnim) {
        return static_cast<s32>(node->compositeAnim->field12);
    }

    if (node->frameList) {
        return node->frameList->numFrames;
    }

    return 0;
}

static MiscAnimNode* Collectible_ResolveCompositePartNode(CompositeAnimPartData& part) {
    if (part.animNameUID == 0) {
        return nullptr;
    }

    if (g_animMgr) {
        MiscAnimNode* liveNode = g_animMgr->GetMiscAnim(part.animNameUID);
        if (liveNode && liveNode != part.animNode) {
            part.animNode = liveNode;
        }
    }

    return part.animNode;
}

static bool Collectible_ResolveSequenceAnimFrame(SequenceAnim* sequence,
                                                 s32 frame,
                                                 MiscAnimNode** outNode,
                                                 TransformAnim** outAnim,
                                                 s32* outFrame) {
    if (!sequence || !sequence->parts || sequence->numParts == 0 || !outNode || !outAnim || !outFrame) {
        return false;
    }

    s32 sequenceFrame = frame;
    for (s32 depth = 0; depth < COLLECT_MISC_ANIM_MAX_DEPTH; depth++) {
        const u32 frameBits = sequence->frameBits;
        const u32 frameMask = (frameBits >= 31u) ? 0x7FFFFFFFu : ((1u << frameBits) - 1u);
        const u32 partIndex = static_cast<u32>(sequenceFrame) >> frameBits;
        const s32 partFrame = sequenceFrame & static_cast<s32>(frameMask);
        if (partIndex >= sequence->numParts) {
            return false;
        }

        SequenceAnimPart& part = sequence->parts[partIndex];
        if (part.animHash != 0 && g_animMgr) {
            MiscAnimNode* liveNode = g_animMgr->GetMiscAnim(part.animHash);
            if (liveNode && liveNode != part.node) {
                part.node = liveNode;
                part.anim = liveNode->anim;
            }
        }

        MiscAnimNode* partNode = part.node;
        if (partNode && partNode->sequenceAnim) {
            sequence = partNode->sequenceAnim;
            if (!sequence || !sequence->parts || sequence->numParts == 0) {
                return false;
            }
            sequenceFrame = partFrame;
            continue;
        }

        if (partNode) {
            *outNode = partNode;
            *outAnim = nullptr;
            *outFrame = partFrame;
            return true;
        }

        if (part.anim) {
            *outNode = nullptr;
            *outAnim = part.anim;
            *outFrame = partFrame;
            return true;
        }

        return false;
    }

    return false;
}

static bool Collectible_ResolveInitialTransformRecursive(MiscAnimNode* node,
                                                         s32 frame,
                                                         TransformAnim** outAnim,
                                                         s32* outFrame,
                                                         s32 depth) {
    if (!node || !outAnim || !outFrame || depth >= COLLECT_MISC_ANIM_MAX_DEPTH) {
        return false;
    }

    if (node->anim) {
        *outAnim = node->anim;
        *outFrame = frame;
        return true;
    }

    if (node->sequenceAnim) {
        MiscAnimNode* partNode = nullptr;
        TransformAnim* partAnim = nullptr;
        s32 partFrame = 0;
        if (!Collectible_ResolveSequenceAnimFrame(node->sequenceAnim, frame, &partNode, &partAnim, &partFrame)) {
            return false;
        }

        if (partNode) {
            return Collectible_ResolveInitialTransformRecursive(partNode, partFrame, outAnim, outFrame, depth + 1);
        }

        if (partAnim) {
            *outAnim = partAnim;
            *outFrame = partFrame;
            return true;
        }

        return false;
    }

    if (node->compositeAnim && node->compositeAnim->parts) {
        for (u32 i = 0; i < node->compositeAnim->numParts; i++) {
            CompositeAnimPartData& part = node->compositeAnim->parts[i];
            MiscAnimNode* partNode = Collectible_ResolveCompositePartNode(part);
            if (!partNode) {
                continue;
            }

            const s32 partFrameCount = Collectible_GetMiscAnimFrameCount(partNode);
            if (partFrameCount <= 0) {
                continue;
            }

            const u32 shift = static_cast<u32>(part.field1) & 31u;
            s32 partFrame = frame >> shift;
            if (partFrame >= partFrameCount) {
                if (part.field0 != 0) {
                    partFrame %= partFrameCount;
                }
                else {
                    partFrame = partFrameCount - 1;
                }
            }

            if (Collectible_ResolveInitialTransformRecursive(partNode, partFrame, outAnim, outFrame, depth + 1)) {
                return true;
            }
        }
    }

    return false;
}

static bool Collectible_ResolveInitialTransform(MiscAnimNode* node, s32 frame, TransformAnim** outAnim, s32* outFrame) {
    if (!outAnim || !outFrame) {
        return false;
    }

    *outAnim = nullptr;
    *outFrame = 0;
    return Collectible_ResolveInitialTransformRecursive(node, frame, outAnim, outFrame, 0);
}

static bool Collectible_BindTransformAnim(Collectible* collectible, TransformAnim* anim) {
    if (!collectible || !anim || !collectible->model) {
        return false;
    }

    Model* mdl = static_cast<Model*>(collectible->model);
    AnimStructure* animStructure = static_cast<AnimStructure*>(mdl->animStructure);
    if (!animStructure || !animStructure->flip) {
        return false;
    }

    if (collectible->mBoundTransformAnim != anim) {
        animStructure->animation = anim;
        animStructure->flip->anim = anim;
        animStructure->flip->dirty = 1;
        animStructure->ResetCountsToAnim();
        collectible->mBoundTransformAnim = anim;
    }

    collectible->mAnim = animStructure;
    return true;
}

static bool Collectible_ApplyTransformAnimFrame(Collectible* collectible, TransformAnim* anim, s32 frame, bool updateJoints) {
    if (!Collectible_BindTransformAnim(collectible, anim)) {
        return false;
    }

    AnimStructure* animStructure = collectible->mAnim;
    if (!animStructure || !animStructure->flip) {
        return false;
    }

    animStructure->flip->SetFrame(frame);
    if (updateJoints) {
        animStructure->flip->UpdateJoints();
    }

    return true;
}

static bool Collectible_ApplyMiscAnimFrameRecursive(Collectible* collectible,
                                                     MiscAnimNode* node,
                                                     s32 frame,
                                                     bool updateJoints,
                                                     s32 depth) {
    if (!collectible || !node || depth >= COLLECT_MISC_ANIM_MAX_DEPTH) {
        return false;
    }

    if (node->anim) {
        return Collectible_ApplyTransformAnimFrame(collectible, node->anim, frame, updateJoints);
    }

    if (node->sequenceAnim) {
        MiscAnimNode* partNode = nullptr;
        TransformAnim* partAnim = nullptr;
        s32 partFrame = 0;
        if (!Collectible_ResolveSequenceAnimFrame(node->sequenceAnim, frame, &partNode, &partAnim, &partFrame)) {
            return false;
        }

        if (partNode) {
            return Collectible_ApplyMiscAnimFrameRecursive(collectible, partNode, partFrame, updateJoints, depth + 1);
        }

        if (partAnim) {
            return Collectible_ApplyTransformAnimFrame(collectible, partAnim, partFrame, updateJoints);
        }

        return false;
    }

    if (node->compositeAnim && node->compositeAnim->parts) {
        bool applied = false;
        bool resolvedAnyPart = false;
        for (u32 i = 0; i < node->compositeAnim->numParts; i++) {
            CompositeAnimPartData& part = node->compositeAnim->parts[i];
            MiscAnimNode* partNode = Collectible_ResolveCompositePartNode(part);
            if (!partNode) {
                continue;
            }

            resolvedAnyPart = true;

            const s32 partFrameCount = Collectible_GetMiscAnimFrameCount(partNode);
            if (partFrameCount <= 0) {
                continue;
            }

            const u32 shift = static_cast<u32>(part.field1) & 31u;
            s32 partFrame = frame >> shift;
            if (partFrame >= partFrameCount) {
                if (part.field0 != 0) {
                    partFrame %= partFrameCount;
                }
                else {
                    partFrame = partFrameCount - 1;
                }
            }

            if (Collectible_ApplyMiscAnimFrameRecursive(collectible, partNode, partFrame, updateJoints, depth + 1)) {
                applied = true;
            }
        }

        if (!resolvedAnyPart) {
            return true;
        }

        return applied;
    }

    if (node->paramAnim || node->frameList) {
        return true;
    }

    return false;
}

static bool Collectible_ApplyMiscAnimFrame(Collectible* collectible, MiscAnimNode* node, s32 frame, bool updateJoints) {
    return Collectible_ApplyMiscAnimFrameRecursive(collectible, node, frame, updateJoints, 0);
}

static bool Collectible_ShouldDriveMiscAnim(const MiscAnimNode* node) {
    if (!node) {
        return false;
    }

    // PSX collectible special handling branch is composite-root specific.
    return node->compositeAnim != nullptr;
}

// PSX CreateModel branch (0x80012B18..0x80012BCC): if current collectible
// animation root is composite, scan parts for the first tTreeFlip (0x20003)
// and attach it to the model tree.
static void Collectible_AttachCompositeTreeFlipPart(Collectible* collectible, MiscAnimNode* rootNode) {
    if (!collectible || !rootNode) {
        return;
    }

    if (Collectible_GetMiscEntityType(rootNode) != COLLECT_ENTITY_TYPE_COMPOSITE_ANIM) {
        return;
    }

    AnimStructure* animStructure = collectible->mAnim;
    if (!animStructure || !animStructure->flip || !animStructure->flip->tree) {
        return;
    }

    CompositeAnimData* composite = rootNode->compositeAnim;
    if (!composite || !composite->parts) {
        return;
    }

    for (u32 partIndex = 0; partIndex < composite->numParts; partIndex++) {
        CompositeAnimPartData& part = composite->parts[partIndex];
        MiscAnimNode* partNode = Collectible_ResolveCompositePartNode(part);
        if (!partNode || Collectible_GetMiscEntityType(partNode) != COLLECT_ENTITY_TYPE_TREE_FLIP || !partNode->anim) {
            continue;
        }

        animStructure->animation = partNode->anim;
        animStructure->flip->Attach(animStructure->flip->tree, partNode->anim);
        collectible->mBoundTransformAnim = partNode->anim;
        break;
    }
}

Collectible::Collectible(const LVector* pos, u16 type)
    : Obstacle(pos, type) {
    MARKFUNCTION(0x80012744);

    mAnim = nullptr;
    mAnimB = nullptr;
    mBoundTransformAnim = nullptr;
    mCurrentFrame = 0;
    mModelIndex = -1;
    mFloatAngle = 0;
    mFlipAngle = 1;
    mTimer = 20;
}

Collectible::~Collectible() {
    MARKFUNCTION(0x800127A4);
}

void Collectible::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x800127CC);

    Obstacle::AnalyzeMesh(root);

    root->FindAttribValue(21, reinterpret_cast<u32*>(&mModelIndex));

    orientation.x = root->field40;
    orientation.y = root->field44;
    orientation.z = root->field48;

    tagCollisionBox localBox = { 0x7FFF, 0x7FFF, 0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0 };

    if (root->FindAttribValue(21, reinterpret_cast<u32*>(&mModelIndex))) {
        FillCollisionBox(localBox, *static_cast<const DBVolume*>(root));
    }
    else {
        mModelIndex = -1;
        ObstacleFillCollisionBox(localBox, root, 5);
    }

    // PC: enlarge the pickup box a little so collectibles are easier to catch.
    if (CollectibleBoxLooksValid(localBox)) {
        auto pad = [](s16 v, s32 amount, bool grow) -> s16 {
            const s32 r = grow ? (v + amount) : (v - amount);
            return static_cast<s16>(r < -0x7FFF ? -0x7FFF : (r > 0x7FFF ? 0x7FFF : r));
        };
        localBox.minX = pad(localBox.minX, COLLECT_PICKUP_PAD_XZ, false);
        localBox.minZ = pad(localBox.minZ, COLLECT_PICKUP_PAD_XZ, false);
        localBox.maxX = pad(localBox.maxX, COLLECT_PICKUP_PAD_XZ, true);
        localBox.maxZ = pad(localBox.maxZ, COLLECT_PICKUP_PAD_XZ, true);
        localBox.minY = pad(localBox.minY, COLLECT_PICKUP_PAD_Y, false);
        localBox.maxY = pad(localBox.maxY, COLLECT_PICKUP_PAD_Y, true);
    }

    SetCollisionBox(localBox);

    const DBAttrib* attrib = root->FindAttrib(6);
    mCollectible = (attrib && attrib->type == 1) ? static_cast<s32>(attrib->value) : 0;

    attrib = root->FindAttrib(7);
    mPointsToAdd = (attrib && attrib->type == 1) ? static_cast<s32>(attrib->value) : 0;

    attrib = root->FindAttrib(8);
    mHealthToAdd = (attrib && attrib->type == 1) ? static_cast<s32>(attrib->value) : 0;

    attrib = root->FindAttrib(9);
    mLivesToAdd = (attrib && attrib->type == 1) ? static_cast<s32>(attrib->value) : 0;

    if (mCollectible != 0) {
        if (g_scoreManager && g_scoreManager->RegisterCollectible(this, mCollectible) != 0) {
            Kill();
        }
    }
}

void Collectible::CreateModel(const char* name) {
    MARKFUNCTION(0x80012970);

    mBoundTransformAnim = nullptr;

    if (mModelIndex != -1 && modelHash != 0) {
        Thing::CreateModel(nullptr);

        Model* mdl = static_cast<Model*>(model);
        if (mdl) {
            mdl->modelFlags |= 5u;
        }

        mAnimB = Obstacle_GetAnimation(mModelIndex);
        if (mAnimB && mdl) {
            TransformAnim* initialAnim = mAnimB->anim;
            s32 initialFrame = 0;
            if (!initialAnim) {
                Collectible_ResolveInitialTransform(mAnimB, 0, &initialAnim, &initialFrame);
            }

            if (initialAnim) {
                if (mdl->drawableType == 2) {
                    AnimStructure* as = new AnimStructure(2, initialAnim, ANIM_HOLD_LAST, mdl, mdl->drawable);
                    mAnim = as;
                    mdl->animStructure = as;

                    if (as) {
                        SModel* sm = static_cast<SModel*>(mdl);
                        sm->ApplyAnimToModelBasic(initialAnim);
                        mAnim = static_cast<AnimStructure*>(sm->animStructure);
                        mCurrentFrame = 0;
                        mBoundTransformAnim = initialAnim;
                        if (mAnim && mAnim->flip) {
                            mAnim->flip->SetFrame(initialFrame);
                            mAnim->flip->UpdateJoints();
                        }
                    }
                }
                else if (mdl->drawableType == 3) {
                    delete static_cast<AnimStructure*>(mdl->animStructure);
                    mdl->animStructure = new AnimStructure(2, initialAnim, ANIM_HOLD_LAST, mdl, mdl->drawable);
                    mAnim = static_cast<AnimStructure*>(mdl->animStructure);
                    mCurrentFrame = 0;
                    mBoundTransformAnim = initialAnim;
                    if (mAnim && mAnim->flip) {
                        mAnim->flip->SetFrame(initialFrame);
                        mAnim->flip->UpdateJoints();
                    }
                }
            }
            else {
                mAnim = static_cast<AnimStructure*>(mdl->animStructure);
            }

            Collectible_AttachCompositeTreeFlipPart(this, mAnimB);

            if (Collectible_ShouldDriveMiscAnim(mAnimB)) {
                mCurrentFrame = 0;
                Collectible_ApplyMiscAnimFrame(this, mAnimB, 0, true);
            }
        }
    }
    else {
        if (!model) {
            GModel* gm = new GModel();
            gm->backPtr = this;
            gm->modelFlags |= 5u;
            model = gm;
        }

        Thing::CreateModel(name);

        Model* mdl = static_cast<Model*>(model);
        if (mdl) {
            mdl->modelFlags |= 5u;
        }
    }

    mInitialPos = pos;
    AllocateAndCreateShadow();
}

void Collectible::DeleteModel() {
    MARKFUNCTION(0x80012C5C);
    mBoundTransformAnim = nullptr;
    Thing::DeleteModel();
}

void Collectible::Reset() {
    MARKFUNCTION(0x80012C7C);
}

void Collectible::Think() {
    MARKFUNCTION(0x80012C84);

    if (mTimer > 0) {
        mTimer--;
    }

#if HIGH_FPS_PLAY_PRESENTATION
    const s32 frameBeforeTick = mCurrentFrame;
#endif

    if (Collectible_ShouldDriveMiscAnim(mAnimB)) {
        const s32 frameCount = Collectible_GetMiscAnimFrameCount(mAnimB);
        if (frameCount > 0 && mCurrentFrame < frameCount) {
            s32 oldFrame = mCurrentFrame;
            mCurrentFrame = oldFrame + 1;
            Collectible_ApplyMiscAnimFrame(this, mAnimB, oldFrame, true);
        }
        else {
            mCurrentFrame = 0;
        }
    }
    else if (mAnim && mAnim->flip && mAnim->flip->anim) {
        TransformFlip* flip = mAnim->flip;
        if (mCurrentFrame < flip->anim->numFrames) {
            s32 oldFrame = mCurrentFrame;
            mCurrentFrame = oldFrame + 1;
            flip->SetFrame(oldFrame);
            flip->UpdateJoints();
        }
        else {
            mCurrentFrame = 0;
        }
    }
    else {
        mCurrentFrame = 0;
    }

#if HIGH_FPS_PLAY_PRESENTATION
    mRenderPrevFrame = frameBeforeTick;
#endif

    const s32 bobOffset = MulShift16(rmSin16(mFloatAngle + 0x4000), 16);
    const s32 waveRadius = bobOffset + 32;

    if (mFlipAngle != 0) {
        pos.x = mInitialPos.x + MulShift16(rmSin16(mFloatAngle + 0x4000), waveRadius);
        pos.y = mInitialPos.y + MulShift16(rmSin16(mFloatAngle), waveRadius);

        if (mFloatAngle > 0xFFFF) {
            mFloatAngle = -0x8000;
            mFlipAngle ^= 1;
        }
        else {
            mFloatAngle += COLLECT_FLOAT_STEP;
        }
    }
    else {
        pos.x = mInitialPos.x + MulShift16(rmSin16(mFloatAngle + 0x4000), 32) + 64;
        pos.y = mInitialPos.y + MulShift16(rmSin16(mFloatAngle), waveRadius);

        if (-mFloatAngle > 0x17FFF) {
            mFloatAngle = 0;
            mFlipAngle ^= 1;
        }
        else {
            mFloatAngle -= COLLECT_FLOAT_STEP;
        }
    }

    pos.z = mInitialPos.z;
    UpdateShadowFloorHeight();
}

// PC
void Collectible::Draw() {
#if HIGH_FPS_PLAY_PRESENTATION
    const bool collectibleInPlay = (g_time && g_game && g_game->GetState() == GameState::Play);
    if (collectibleInPlay) {
        f32 alpha = g_time->GetPlayPresentationAlpha();
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;

        // Snap on loop wrap.
        if (mCurrentFrame < mRenderPrevFrame) {
            alpha = 1.0f;
        }

        const s32 frameStep = mCurrentFrame - mRenderPrevFrame;

        if (Collectible_ShouldDriveMiscAnim(mAnimB)) {
            const s32 interpFrame = mRenderPrevFrame + (s32)((f32)frameStep * alpha + 0.5f);
            Collectible_ApplyMiscAnimFrame(this, mAnimB, interpFrame, true);
        }
        else if (mAnim && mAnim->flip && mAnim->flip->anim) {
            const s32 prevFrameReal = mRenderPrevFrame << 16;
            const s32 curFrameReal = mCurrentFrame << 16;
            const s32 interpFrameReal = prevFrameReal + (s32)((f32)(curFrameReal - prevFrameReal) * alpha);
            mAnim->flip->SetFrameReal(interpFrameReal);
            mAnim->flip->UpdateJoints();
        }
    }
#endif

    Obstacle::Draw();
}

void Collectible::UpdatePosition() {
    MARKFUNCTION(0x80012EC8);
}

void Collectible::HandlePickupCollision(Thing* pickup) {
    MARKFUNCTION(0x80012ED0);
    (void)pickup;
}

void Collectible::HandleHumanoidCollision(Humanoid* hum) {
    MARKFUNCTION(0x80012ED8);

    if (mTimer > 0 || !hum) {
        return;
    }

    if (hum->thingType != AITypes::TT_PLAYER) {
        return;
    }

    if (g_scoreManager) {
        g_scoreManager->RegisterGotCollectible(this, mCollectible);
    }

    if (mCollectible == 1 || mCollectible == 2) {
        const u16 soundID = (mCollectible == 1) ? 254 : 255;
        CSoundDirect::PlayTransient(soundID, &pos, 0, 0);
        hum->LoadDialog(14, 99);
        hum->PlayDialog(14, 60);
    }
    else if (mLivesToAdd != 0) {
        CSoundDirect::PlayTransient(257, &pos, 0, 0);
    }

    if (mHealthToAdd != 0) {
        const s32 newHealth = static_cast<s32>(hum->health) + mHealthToAdd;
        if (newHealth < static_cast<s32>(hum->maxHealth)) {
            hum->health = static_cast<u16>(newHealth);
        }
        else {
            hum->health = hum->maxHealth;
        }
        CSoundDirect::PlayTransient(256, &pos, 0, 0);
    }

    if (mLivesToAdd != 0) {
        if (Player::s_player) {
            Player::s_player->SetLivesLeft(Player::s_player->livesLeft + mLivesToAdd);
        }

        if (g_hud) {
            g_hud->DisplayExtraTake();
        }
    }

    Kill();
}
