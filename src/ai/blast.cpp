#include "ai/blast.h"
#include "ai/humanoid.h"
#include "ai/player.h"
#include "gen/common.h"
#include "gen/database.h"
#include "gen/colvol.h"
#include "gen/effects.h"
#include "gen/geffect.h"
#include "gen/psxmath_helpers.h"
#include "gen/weffect.h"
#include "ai/obstacle_shared.h"
#include "p3d/hash.h"
#include "snd/esound.h"
#include "snd/sndfact.h"
#include "snd/snddrct.h"

static constexpr s32 COLLISION_TAG_HIT_TYPE = static_cast<s32>(0x80000003u);
static constexpr s32 COLLISION_TAG_DAMAGE = static_cast<s32>(0x80000007u);
static constexpr s32 COLLISION_TAG_END = 0;
static constexpr s32 BLAST_HIT_TYPE_FIRE = 18;
static constexpr s32 BLAST_FORCE_SCALE = 1000 << 16;
static constexpr s32 BLAST_DEFAULT_HALF_WIDTH = 0;

static s32 BlastSll16(s32 value) {
    return static_cast<s32>(static_cast<s64>(value) << 16);
}

static s32 BlastAttribValue(const DBRoot* root, u32 id, s32 defaultValue = 0) {
    u32 value = 0;
    return (root && root->FindAttribValue(id, &value)) ? static_cast<s32>(value) : defaultValue;
}

static u32 BlastAttribStringHash(const DBRoot* root, u32 id) {
    if (!root) {
        return 0;
    }

    const DBAttrib* attrib = root->FindAttrib(id);
    const char* str = attrib ? attrib->GetAttribString() : nullptr;
    return str ? p3dHash(str) : 0;
}

static bool BlastUsesFramedEffect(const Blast* blast) {
    return blast && blast->framedEffect != 0;
}

static s16 BlastFrameTableValue(const Blast* blast, s32 index) {
    if (!blast || !blast->effectFrameTable || index < 0 || index >= 8) {
        return 0;
    }

    return blast->effectFrameTable[index];
}

static void SetBlastStateFrame(Blast* blast, s32 state) {
    if (!blast || !blast->effectFrameTable) {
        return;
    }

    blast->effectFrame = BlastFrameTableValue(blast, state * 2);
}

static void AdvanceBlastFrame(Blast* blast) {
    if (!blast || !blast->effect || !blast->effectFrameTable) {
        return;
    }

    blast->effectFrame++;

    s32 state = blast->blastState;
    if (state < 0 || state > 3) {
        return;
    }

    const s32 start = BlastFrameTableValue(blast, state * 2);
    const s32 end = BlastFrameTableValue(blast, state * 2 + 1);
    if (end < blast->effectFrame) {
        blast->effectFrame = start;
    }
}

static void StartBlastFire(Blast* blast) {
    if (!blast || blast->blastState != 0) {
        return;
    }

    blast->blastState = 1;
    blast->stateTimer = 0;

    const u32 effectHash = static_cast<u32>(blast->effectHash);
    if (effectHash && !BlastUsesFramedEffect(blast)) {
        const s32 lifeFrames = blast->extendFrames + blast->holdFrames + blast->retractFrames;
        GEffect_Create(effectHash, &blast->pos, nullptr, nullptr, 0, lifeFrames, 0);
    }

    SetBlastStateFrame(blast, 1);
}

static void BuildBlastCollisionBox(Blast* blast, s32 progress) {
    if (!blast) {
        return;
    }

    const s32 halfWidth = blast->halfWidth;
    const s32 length = progress;

    tagCollisionBox box = {};
    box.minZ = 0;
    box.maxZ = 0;

    switch (blast->majorAxis) {
        case 0:
            box.minY = static_cast<s16>(-halfWidth);
            box.maxY = static_cast<s16>(halfWidth);
            box.minZ = static_cast<s16>(-halfWidth);
            box.maxZ = static_cast<s16>(halfWidth);
            if (length > 0) {
                box.maxX = static_cast<s16>(length);
            }
            else {
                box.minX = static_cast<s16>(length);
            }
            break;
        case 1:
            box.minX = static_cast<s16>(-halfWidth);
            box.maxX = static_cast<s16>(halfWidth);
            box.minZ = static_cast<s16>(-halfWidth);
            box.maxZ = static_cast<s16>(halfWidth);
            if (length > 0) {
                box.minY = 0;
                box.maxY = static_cast<s16>(length);
            }
            else {
                box.minY = static_cast<s16>(length);
                box.maxY = 0;
            }
            break;
        default:
            box.minX = static_cast<s16>(-halfWidth);
            box.maxX = static_cast<s16>(halfWidth);
            box.minY = static_cast<s16>(-halfWidth);
            box.maxY = static_cast<s16>(halfWidth);
            if (length > 0) {
                box.minZ = 0;
                box.maxZ = static_cast<s16>(length);
            }
            else {
                box.minZ = static_cast<s16>(length);
                box.maxZ = 0;
            }
            break;
    }

    blast->SetCollisionBox(box);
}

static s32 ScaleBlastForce(s32 value) {
    return PsxRmDiv16i(value, BLAST_FORCE_SCALE);
}

static LVector NormalizeBlastDirection(s32 x, s32 y, s32 z) {
    const s32 sx = BlastSll16(x);
    const s32 sy = BlastSll16(y);
    const s32 sz = BlastSll16(z);
    const s32 mag = rmMag3ff(sx, sy, sz);
    if (mag == 0) {
        return { FIX16_ONE, 0, 0 };
    }

    return {
        PsxRmDiv16i(sx, mag),
        PsxRmDiv16i(sy, mag),
        PsxRmDiv16i(sz, mag),
    };
}

static s32 ScaleBlastRatioByFrames(s32 ratio, s32 frames) {
    return MulShift16(ratio, frames);
}

static bool ApplyBlastWindForce(Blast* blast, Humanoid* hum) {
    if (!blast || !hum) {
        return false;
    }

    const s32 blastLength = rmMag3ff(
        blast->endPosX - blast->pos.x,
        blast->endPosY - blast->pos.y,
        blast->endPosZ - blast->pos.z);

    const s32 humDistance = rmMag3ff(
        hum->pos.x - blast->pos.x,
        hum->pos.y - blast->pos.y,
        hum->pos.z - blast->pos.z);

    s32 t = PsxRmDiv16i(BlastSll16(humDistance), BlastSll16(blastLength));
    if (t > FIX16_ONE) {
        t = FIX16_ONE;
    }

    if (blast->blastState == 1 && blast->extendFrames != 0) {
        t = PsxRmDiv16i(ScaleBlastRatioByFrames(t, static_cast<s16>(blast->stateTimer)), blast->extendFrames);
    }
    else if (blast->blastState == 3 && blast->retractFrames != 0) {
        t = PsxRmDiv16i(ScaleBlastRatioByFrames(t, static_cast<s16>(blast->stateTimer)), blast->retractFrames);
    }

    const s32 invT = FIX16_ONE - t;
    const s32 forceX = MulShift16(blast->minForceX, invT) + MulShift16(blast->maxForceX, t);
    const s32 forceY = MulShift16(blast->minForceY, invT) + MulShift16(blast->maxForceY, t);
    const s32 forceZ = MulShift16(blast->minForceZ, invT) + MulShift16(blast->maxForceZ, t);

    hum->contactForce.x += forceX;
    hum->contactForce.y += forceY;
    hum->contactForce.z += forceZ;

    return forceX != 0 || forceY != 0 || forceZ != 0;
}

Blast::Blast(const LVector* pos, u16 type)
    : Obstacle(pos, type) {
    MARKFUNCTION(0x80015AB0);
}


Blast::~Blast() {
    MARKFUNCTION(0x80015AF8);
    delete[] effectFrameTable;
    effectFrameTable = nullptr;
}

void Blast::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x80015B54);
    Obstacle::AnalyzeMesh(root);

    orientation = {};

    tagCollisionBox localBox = { 0x7FFF, 0x7FFF, 0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0 };
    ObstacleFillCollisionBox(localBox, root, 5);
    SetCollisionBox(localBox);

    cooldownFrames = static_cast<s16>(BlastAttribValue(root, 9));
    extendFrames = static_cast<s16>(BlastAttribValue(root, 10));
    holdFrames = static_cast<s16>(BlastAttribValue(root, 11));
    halfWidth = BlastAttribValue(root, 14, BLAST_DEFAULT_HALF_WIDTH);
    damagePreset = BlastAttribValue(root, 21);
    requireTrigger = BlastAttribValue(root, 22);
    initialTimerAdvance = static_cast<s16>(BlastAttribValue(root, 32));

    const s32 blastDirX = BlastAttribValue(root, 6);
    const s32 blastDirY = BlastAttribValue(root, 7);
    const s32 blastDirZ = BlastAttribValue(root, 8);
    endPosX = pos.x + blastDirX;
    endPosY = pos.y + blastDirY;
    endPosZ = pos.z + blastDirZ;

    if (blastDirX != 0) {
        majorAxis = 0;
        lengthPerFrame = extendFrames != 0 ? blastDirX / extendFrames : blastDirX;
    }
    else if (blastDirY != 0) {
        majorAxis = 1;
        lengthPerFrame = extendFrames != 0 ? blastDirY / extendFrames : blastDirY;
    }
    else {
        majorAxis = 2;
        lengthPerFrame = extendFrames != 0 ? blastDirZ / extendFrames : blastDirZ;
    }

    const LVector normalizedDirection = NormalizeBlastDirection(blastDirX, blastDirY, blastDirZ);

    const s32 forceMin = BlastAttribValue(root, 12);
    const s32 forceMax = BlastAttribValue(root, 13);

    minForceX = MulShift16(normalizedDirection.x, forceMin) << 16;
    minForceY = MulShift16(normalizedDirection.y, forceMin) << 16;
    minForceZ = MulShift16(normalizedDirection.z, forceMin) << 16;
    maxForceX = MulShift16(normalizedDirection.x, forceMax) << 16;
    maxForceY = MulShift16(normalizedDirection.y, forceMax) << 16;
    maxForceZ = MulShift16(normalizedDirection.z, forceMax) << 16;

    u32 rawForceX = 0;
    if (root && root->FindAttribValue(16, &rawForceX)) {
        minForceX = BlastSll16(static_cast<s32>(rawForceX));
        maxForceX = BlastSll16(static_cast<s32>(rawForceX));
    }
    u32 rawForceY = 0;
    if (root && root->FindAttribValue(17, &rawForceY)) {
        minForceY = BlastSll16(static_cast<s32>(rawForceY));
        maxForceY = BlastSll16(static_cast<s32>(rawForceY));
    }
    u32 rawForceZ = 0;
    if (root && root->FindAttribValue(18, &rawForceZ)) {
        minForceZ = BlastSll16(static_cast<s32>(rawForceZ));
        maxForceZ = BlastSll16(static_cast<s32>(rawForceZ));
    }

    minForceX = ScaleBlastForce(minForceX);
    minForceY = ScaleBlastForce(minForceY);
    minForceZ = ScaleBlastForce(minForceZ);
    maxForceX = ScaleBlastForce(maxForceX);
    maxForceY = ScaleBlastForce(maxForceY);
    maxForceZ = ScaleBlastForce(maxForceZ);

    effectHash = static_cast<s32>(BlastAttribStringHash(root, 20));
    framedEffect = (BlastAttribValue(root, 23) != 0 && effectHash != 0) ? 1 : 0;

    delete[] effectFrameTable;
    effectFrameTable = nullptr;
    if (BlastUsesFramedEffect(this)) {
        effectFrameTable = new s16[8]();
        for (s32 i = 0; i < 8; i++) {
            effectFrameTable[i] = static_cast<s16>(BlastAttribValue(root, 24 + i));
        }
        retractFrames = extendFrames;
    }
    else {
        retractFrames = 0;
    }

    switch (damagePreset) {
        case 0:
            collisionDamage = 0;
            break;
        case 1:
            collisionDamage = 3;
            break;
        case 2:
            collisionDamage = 6;
            break;
        case 3:
            collisionDamage = 1;
            break;
        case 4:
            collisionDamage = 4;
            break;
        default:
            collisionDamage = 0;
            break;
    }
    hitCooldownFrames = 2;
    hitCooldownTimer = hitCooldownFrames;
    stateTimer = cooldownFrames - initialTimerAdvance;
    blastState = 0;
    SetBlastStateFrame(this, 0);
}

void Blast::CreateSound() {
    MARKFUNCTION(0x80016284);
    if (sound) {
        return;
    }
    if (!effect) {
        return;
    }
    CSound* soundObj = nullptr;
    const u32 soundId = effect ? effect->resourceHash : 0;
    if (CSoundFactory::CreateObject(10010, &soundObj, soundId) >= 0) {
        sound = static_cast<CWorldEffectSound*>(soundObj);
        if (sound) {
            sound->Initialize(&pos);
        }
    }
}

void Blast::UpdateSound() {
    MARKFUNCTION(0x800162E4);
    if (sound) {
        sound->Update((u32)effectFrame);
    }
}

void Blast::ReleaseSound() {
    MARKFUNCTION(0x8001631C);
    if (sound) {
        sound->Release();
        sound = nullptr;
    }
}

void Blast::CreateModel(const char* name) {
    MARKFUNCTION(0x80016368);
    flags |= TF_MODEL_CREATED;
    (void)name;
    if (BlastUsesFramedEffect(this)) {
        CreateSound();
    }
}

void Blast::DeleteModel() {
    MARKFUNCTION(0x8001639C);
    flags &= ~TF_MODEL_CREATED;
    ReleaseSound();
}

void Blast::Reset() {
    MARKFUNCTION(0x800163C8);
    blastState = 0;
    stateTimer = cooldownFrames - initialTimerAdvance;
    if (BlastUsesFramedEffect(this) && effectHash) {
        FWEffect* fwEffect = FWEffect::Find(static_cast<u32>(effectHash));
        if (fwEffect) {
            effect = fwEffect->comEffect;
            effectRenderFlags = static_cast<s32>(fwEffect->renderFlags);
            orientation = fwEffect->rotation;
            effectScale = fwEffect->scale;
        }
        else {
            effect = nullptr;
            effectRenderFlags = 0;
            orientation = {};
            effectScale = { 0x10000, 0x10000, 0x10000 };
        }
        effectHash = 0;
    }

    SetBlastStateFrame(this, 0);
    lastEffectFrame = effectFrame;
    hitCooldownTimer = hitCooldownFrames;
    ReleaseSound();
}

void Blast::Activate() {
    MARKFUNCTION(0x800164A4);
    Thing::Activate();
}

void Blast::Deactivate() {
    MARKFUNCTION(0x800164C4);
    Thing::Deactivate();
}

void Blast::Think() {
    MARKFUNCTION(0x80016528);

    s32 progress = 0;

    AdvanceBlastFrame(this);

    if (blastState == 0) {
        if (stateTimer >= cooldownFrames && requireTrigger == 0) {
            StartBlastFire(this);
        }
    }
    else if (blastState == 1) {
        progress = lengthPerFrame * static_cast<s32>(stateTimer);
        if (stateTimer >= extendFrames) {
            blastState = 2;
            stateTimer = 0;
            SetBlastStateFrame(this, 2);
        }
    }
    else if (blastState == 2) {
        progress = lengthPerFrame * static_cast<s32>(extendFrames);
        if (stateTimer >= holdFrames) {
            if (retractFrames > 0) {
                blastState = 3;
                stateTimer = 0;
                SetBlastStateFrame(this, 3);
            }
            else {
                blastState = 0;
                stateTimer = 0;
                SetBlastStateFrame(this, 0);
            }
        }
    }
    else if (blastState == 3) {
        progress = lengthPerFrame * (static_cast<s32>(retractFrames) - static_cast<s32>(stateTimer));
        if (stateTimer >= retractFrames) {
            blastState = 0;
            stateTimer = 0;
            SetBlastStateFrame(this, 0);
        }
    }

    BuildBlastCollisionBox(this, progress);
    stateTimer++;

    if (hitCooldownTimer <= 0) {
        hitCooldownTimer = hitCooldownFrames;
    }
    hitCooldownTimer--;

    // PSX OL1: Blast::Think loads the player's damage reaction dialog every
    // frame for presets 1 and 2 (D093 and D092, respectively), at priority 60.
    if (Player::s_player) {
        if (damagePreset == 2) {
            Player::s_player->LoadDialog(92, 0x3C);
        }
        else if (damagePreset == 1) {
            Player::s_player->LoadDialog(93, 0x3C);
        }
    }
}

void Blast::Trigger() {
    MARKFUNCTION(0x80016A10);

    if (blastState || stateTimer < cooldownFrames) {
        return;
    }

    StartBlastFire(this);
}

void Blast::Draw() {
    MARKFUNCTION(0x80016ACC);
    if (!effect) {
        return;
    }

    UpdateSound();
    effect->SetFrame(effectFrame);
    effect->Render(pos, &effectScale, &orientation, static_cast<u32>(effectRenderFlags));
    lastEffectFrame = effectFrame;
}

void Blast::HandlePickupCollision(Thing* pickup) {
    MARKFUNCTION(0x80016B3C);
    (void)pickup;
}

void Blast::HandleHumanoidCollision(Humanoid* hum) {
    MARKFUNCTION(0x80016B44);

    if (!blastState) {
        return;
    }

    const bool hasForce = ApplyBlastWindForce(this, hum);

    if (hitCooldownTimer != 0) {
        return;
    }

    if (collisionDamage <= 0) {
        return;
    }

    hum->SubtractHitPoints(static_cast<u16>(collisionDamage));

    // PSX OL1: Blast::HandleHumanoidCollision plays a player reaction line
    // for damage presets 1 and 2 (D093 and D092, respectively).
    if (hum->thingType == AITypes::TT_PLAYER) {
        if (damagePreset == 2) {
            hum->PlayDialog(92, 10);
        }
        else if (damagePreset == 1) {
            hum->PlayDialog(93, 10);
        }
    }

    if (hasForce) {
        hum->HandleCollision(
            this,
            1,
            COLLISION_TAG_HIT_TYPE,
            BLAST_HIT_TYPE_FIRE,
            COLLISION_TAG_END);
    }
    else {
        hum->HandleCollisionSound(BLAST_HIT_TYPE_FIRE);
    }
}
