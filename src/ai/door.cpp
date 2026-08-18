#include "gen/common.h"
#include "ai/door.h"
#include "ai/activezn.h"
#include "ai/humanoid.h"
#include "ai/obstacle_shared.h"
#include "ai/player.h"
#include "gen/ai.h"
#include "gen/director.h"
#include "gen/game.h"
#include "gen/geffect.h"
#include "gen/blockmgr.h"
#include "gen/world.h"
#include "gen/weffect.h"
#include "snd/snddrct.h"
#include "gen/model.h"
#include "gen/database.h"
#include "extra/shadowcsm.h"

static void DoorCreateUnlockEffects(const Door& door) {
    if (door.secondaryModelHash != 0) {
        GEffect_Create(static_cast<u32>(door.secondaryModelHash), nullptr, nullptr, nullptr, 0, 0, 0);
    }

    if (door.tertiaryModelHash != 0) {
        GEffect_Create(static_cast<u32>(door.tertiaryModelHash), nullptr, nullptr, nullptr, 0, 0, 0);
    }
}

Door::Door(const LVector* pos, u16 type) : Obstacle(pos, type) {
    MARKFUNCTION(0x8001AB0C);
    closedBox = INVALID_COLLISION_BOX;
    doorState = 0;
    targetCRC = 0;
    killThingsCRC = 0;
    tertiaryModelHash = 0;
    currentOpen = 0;
    cutsceneTriggered = 0;
}

Door::~Door() {
    MARKFUNCTION(0x8001AB8C);
}

void Door::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x8001ABB4);

    if (!root) {
        return;
    }

    tagCollisionBox localBox = INVALID_COLLISION_BOX;

    // PSX: copy root orientation to drawRot, then baseRotY = drawRot.y
    drawRot.x = root->field40;
    drawRot.y = root->field44;
    drawRot.z = root->field48;
    baseRotY = drawRot.y;

    // PSX: copy drawRot to thing orientation before Obstacle::AnalyzeMesh
    orientation = drawRot;

    Obstacle::AnalyzeMesh(root);

    // PSX: FillCollisionBox(localBox, root, 5) - OBSTACLE.CPP:530 (0x8007AF6C)
    bool geoFound = ObstacleFillCollisionBox(localBox, root, 5);
    LOG("Door::AnalyzeMesh geoFound=%d box=(%d,%d,%d,%d,%d,%d)",
        geoFound, localBox.minX, localBox.minY, localBox.minZ,
        localBox.maxX, localBox.maxY, localBox.maxZ);

    // PSX: copy localBox to closedBox (always, even if INVALID)
    closedBox = localBox;

    // PSX: attrib 6 openSpeed
    const DBAttrib* a6 = root->FindAttrib(6);
    if (a6) {
        openSpeed = ((s32)a6->value << 16) / 360;
    }
    else {
        openSpeed = 1638;
    }

    // PSX: attrib 7 direction flag (0=subtract, nonzero=add)
    const DBAttrib* a7 = root->FindAttrib(7);
    if (a7) {
        direction = (s32)a7->value;
    }
    else {
        direction = 0;
    }

    // PSX: ApplyDoorStandingZExtent then SetCollisionBox
    ApplyDoorStandingZExtent(localBox);
    SetCollisionBox(localBox);

    // PSX: attrib 8 killThingsCRC (hash of string name)
    const DBAttrib* a8 = root->FindAttrib(8);
    if (a8 && a8->strValue) {
        killThingsCRC = (s32)p3dHash(a8->strValue);
    }
    else {
        killThingsCRC = 0;
    }

    // PSX: attrib 9 maxOpenDist (degrees binary angle)
    const DBAttrib* a9 = root->FindAttrib(9);
    if (a9) {
        maxOpenDist = ((s32)a9->value << 16) / 360;
    }
    else {
        maxOpenDist = 16384;
    }

    // PSX: attrib 10 targetCRC (hash of string name)
    const DBAttrib* a10 = root->FindAttrib(10);
    if (a10 && a10->strValue) {
        targetCRC = (u32)p3dHash(a10->strValue);
    }
    else {
        targetCRC = 0;
    }

    // PSX: attrib 11 secondaryModelHash (hash of string name)
    const DBAttrib* a11 = root->FindAttrib(11);
    if (a11 && a11->strValue) {
        secondaryModelHash = (s32)p3dHash(a11->strValue);
    }
    else {
        secondaryModelHash = 0;
    }

    // PSX: attrib 12 tertiaryModelHash (hash of string name)
    const DBAttrib* a12 = root->FindAttrib(12);
    if (a12 && a12->strValue) {
        tertiaryModelHash = (s32)p3dHash(a12->strValue);
    }
    else {
        tertiaryModelHash = 0;
    }
}

void Door::CreateModel(const char* name) {
    MARKFUNCTION(0x8001AE88);
    Thing::CreateModel(name);
}

void Door::DeleteModel() {
    MARKFUNCTION(0x8001AEA8);
    Thing::DeleteModel();
}

void Door::Reset() {
    MARKFUNCTION(0x8001AEC8);

    World* world = g_game ? g_game->GetWorld() : nullptr;
    s32 levelID = world ? world->GetCurLevelID() : 0;
    const bool startsUnlocked = levelID != 7 && killThingsCRC == 0;
    if (levelID == 7) {
        doorState = 5;
    }
    else if (startsUnlocked) {
        doorState = 1;
    }
    else {
        doorState = 0;
    }

    // PSX: reset animation and state fields
    currentOpen = 0;
    cutsceneTriggered = 0;
    deathCountdown = 3;
    drawRot.y = baseRotY;

    LOG("[Door::Reset] name=%s state=%d pos=(%d,%d,%d) baseRotY=%d targetCRC=0x%08X model=%p flags=0x%X blockNum=%d",
        GetName() ? GetName() : "null", doorState, pos.x, pos.y, pos.z,
        baseRotY, targetCRC, model, flags, blockNum);

    if (startsUnlocked) {
        DoorCreateUnlockEffects(*this);
    }
}

void Door::Think() {
    MARKFUNCTION(0x8001AF5C);

    World* world = g_game ? g_game->GetWorld() : nullptr;
    s32 levelID = world ? world->GetCurLevelID() : 0;

    if (doorState >= 6) {
        return;
    }

    // PSX: switch(doorState) with jump table for states 0-5
    switch (doorState) {
        case 0:
            // State 0: guarded door - monitor kill target
            DeathCheck();
            break;
        case 1:
        case 2:
        case 4:
            // States 1,2,4: animation states - call Move via vtable
            // PSX: also manages secondary model GEffect on non-hub levels
            Move();
            break;
        case 3:
            // State 3: fully open - model management only (no Move)
            // PSX: hides secondary model GEffect on non-hub levels
            break;
        case 5:
            // State 5: hub-closed - idle, waiting for OpenDoors trigger
            break;
    }
}

void Door::UpdatePosition() {
    MARKFUNCTION(0x8001B058);
}

void Door::Draw() {
    MARKFUNCTION(0x8001B060);
    // PSX: custom draw - copies pos and drawRot to model, then calls Show
    if (model) {
        LVector drawPos = pos;
        LVector renderRot = drawRot;

#if MODERN_GRAPHICS
        // See Obstacle::Draw: skip the HIGH_FPS smoothing update during the
        // shadow caster prepass, since this frame's main pass Draw() call
        // still runs afterwards and would double-advance it.
        if (!ShadowCSM::IsCasterPrepass()) {
            ObstacleBuildRenderTransform(this, pos, drawRot, drawPos, renderRot);
        }
#else
        ObstacleBuildRenderTransform(this, pos, drawRot, drawPos, renderRot);
#endif

        Model* m = static_cast<Model*>(model);
        m->posX = drawPos.x;
        m->posY = drawPos.y;
        m->posZ = drawPos.z;
        m->rotX = (u16)(renderRot.x & 0xFFFF);
        m->rotY = (u16)(renderRot.y & 0xFFFF);
        m->rotZ = (u16)(renderRot.z & 0xFFFF);
        m->Show(0);
    }
}

void Door::Trigger() {
    MARKFUNCTION(0x8001B0D4);

    // PSX: always set state 2 (opening)
    doorState = 2;

    World* world = g_game ? g_game->GetWorld() : nullptr;
    s32 levelID = world ? world->GetCurLevelID() : 0;
    if (levelID == 7) {
        // Hub level: chain trigger to target door/teleporter
        if (targetCRC != 0 && g_ai) {
            ccNode* target = g_ai->moveList.FindNodeCRC(targetCRC);
            if (target) {
                Thing* targetThing = static_cast<Thing*>(target);
                u16 targetType = targetThing->thingType;
                LOG("[Door::Trigger] hub chain: targetCRC=0x%08X targetType=%u name=%s",
                    targetCRC, targetType, target->GetName() ? target->GetName() : "null");
                if (targetType == AITypes::TT_LADDER) {
                    Obstacle* obs = static_cast<Obstacle*>(targetThing);
                    obs->Trigger();
                }
                else if (targetType == AITypes::TT_PLATFORM) {
                    Obstacle* obs = static_cast<Obstacle*>(targetThing);
                    obs->TriggerByName(Player::s_player, nullptr, nullptr);
                }
            }
        }

        // PSX: if attrib 11 hash is set, continue the matching FWEffect.
        if (secondaryModelHash != 0) {
            FWEffect* effect = FWEffect::Find((u32)secondaryModelHash);
            LOG("[Door::Trigger] hub FW continue hash=0x%08X found=%d", (u32)secondaryModelHash, effect ? 1 : 0);
            if (effect) {
                effect->Continue();
            }
        }
    }
    else {
        // Non-hub: play door open sound
        CSoundDirect::PlayTransient(156, static_cast<void*>(&pos), 0, 0);
    }
}

void Door::Open() {
    MARKFUNCTION(0x8001B1D8);
    // PSX Open__4Door: doorState = 2 and transient SFX 156.
    doorState = 2;
    CSoundDirect::PlayTransient(156, nullptr, 0, 0);
}

void Door::Move() {
    MARKFUNCTION(0x8001B210);

    // PSX: animate door based on state
    if (doorState == 2) {
        // Opening: increase currentOpen toward maxOpenDist
        currentOpen += openSpeed;
        if (currentOpen >= maxOpenDist) {
            currentOpen = maxOpenDist;
            doorState = 3; // Fully open
        }
    }
    else if (doorState == 4) {
        // Closing: decrease currentOpen toward 0
        currentOpen -= openSpeed;
        if (currentOpen <= 0) {
            currentOpen = 0;
            doorState = 5; // Closed (hub-idle state)
            // PSX: play close sound on non-hub levels
            World* closeWorld = g_game ? g_game->GetWorld() : nullptr;
            s32 levelID = closeWorld ? closeWorld->GetCurLevelID() : 0;
            if (levelID != 7) {
                CSoundDirect::PlayTransient(157, static_cast<void*>(&pos), 0, 3000);
            }
        }
    }

    // PSX: update drawRotY based on direction flag
    if (direction != 0) {
        drawRot.y = baseRotY + currentOpen;
    }
    else {
        drawRot.y = baseRotY - currentOpen;
    }
}

void Door::HandlePickupCollision(Thing* pickup) {
    MARKFUNCTION(0x8001B43C);
    // PSX: calls Pickup::PlayEffect() then pickup->Kill()
    if (pickup) {
        pickup->Kill();
    }
}

void Door::HandleHumanoidCollision(Humanoid* hum) {
    MARKFUNCTION(0x8001B47C);

    if (!hum) {
        return;
    }

    // PSX: if cutsceneTriggered == 1, return (don't push during cutscene)
    if (cutsceneTriggered == 1) {
        return;
    }

    s32 isValidPlayer = 0;
    if (hum->thingType == 0) {
        const s32 actionState = hum->actionState;
        if (actionState == 1 || actionState == 2 || actionState == 3
            || actionState == 10 || actionState == 11) {
            const s32 commandBits = hum->commandBits;
            if (((commandBits >> 7) & 1) || ((commandBits >> 15) & 1)) {
                isValidPlayer = 1;
            }
        }
    }

    if (isValidPlayer && doorState == 1) {
        // Start the actual opening transition before any director cutscene logic.
        doorState = 2;
        CSoundDirect::PlayTransient(156, static_cast<void*>(&pos), 0, 0);

        // PSX: state 1 + valid player  trigger door cutscene via Director
        if (g_director) {
            s32* doorScript = Director::GetNISDoor1Script();
            g_director->SetCodeSnip(doorScript, this);
        }
        cutsceneTriggered = 1;
        return;
    }

    // PSX: push-back via CorrectThingPosition using closedBox and drawRotY
    LVector correctedPos = {};
    LVector correctionNormal = {};
    LVector correctionPushedPos = {};

    CorrectThingPositionObstacle(
        pos,                    // basisA (door pos)
        pos,                    // basisB (door pos)
        drawRot.y,              // rotA (current door rotation)
        drawRot.y,              // rotB (current door rotation)
        closedBox,              // collision box
        hum->pos,               // pointA (humanoid current pos)
        hum->homePos,           // pointB (humanoid home pos)
        hum->collBboxMin.x,    // radius
        hum->collBboxMin.y,    // yMinOffset
        hum->collBboxMin.z,    // yMaxOffset
        correctedPos,           // output: corrected pos
        correctionNormal,       // output: normal
        correctionPushedPos);   // output: pushed pos

    // PSX: copy corrected position to humanoid homePos
    hum->homePos = correctedPos;

    // PSX: if doorState == 1 and hum is the player, set flag bit 6
    if (doorState == 1 && hum == (Humanoid*)Player::s_player) {
        hum->field368 |= 0x40;
    }
}

void Door::TeleportPlayer() {
    MARKFUNCTION(0x8001B624);

    LOG("[Door::TeleportPlayer] targetCRC=0x%08X", targetCRC);

    // PSX: find target by targetCRC in g_ai->moveList
    // Then call target->HandleHumanoidCollision(g_player)
    if (targetCRC != 0 && g_ai) {
        ccNode* target = g_ai->moveList.FindNodeCRC(targetCRC);
        if (target) {
            Thing* tt = static_cast<Thing*>(target);
            LOG("[Door::TeleportPlayer] found target type=%u name=%s", tt->thingType,
                target->GetName() ? target->GetName() : "null");
            Obstacle* obs = static_cast<Obstacle*>(tt);
            obs->HandleHumanoidCollision(Player::s_player);
        }
        else {
            LOG("[Door::TeleportPlayer] target NOT FOUND for CRC 0x%08X", targetCRC);
        }
    }

    if (g_blockManager) {
        g_blockManager->DemandLoading();
    }

    // PSX: clear cutsceneTriggered flag
    cutsceneTriggered = 0;
}

void Door::DeathCheck() {
    MARKFUNCTION(0x8001B2FC);

    s32 isDead = 0;

    // PSX: if killTarget is null, find it by killThingsCRC in g_ai->activeZoneList
    if (!killTarget) {
        if (killThingsCRC != 0 && g_ai) {
            killTarget = static_cast<Thing*>(g_ai->activeZoneList.FindNodeCRC((u32)killThingsCRC));
        }
        if (!killTarget) {
            isDead = 1; // Target not found  consider dead
        }
    }
    else {
        // PSX: check if ActiveZone still has thinking (alive) members
        // 0x800A6E30 = ActiveZone::GetNumberOfThinkingMembers
        ActiveZone* az = static_cast<ActiveZone*>(static_cast<ccNode*>(killTarget));
        s32 thinkingCount = az->GetNumberOfThinkingMembers();

        if (thinkingCount > 0) {
            // Still alive - reset countdown
            deathCountdown = 3;
        }
        else {
            // No thinking members - decrement countdown
            deathCountdown--;
            if (deathCountdown <= 0) {
                isDead = 1;
            }
        }
    }

    if (isDead) {
        // PSX: if level != 6, set state 1 + SFX 158; if level 6, call Open
        World* deathWorld = g_game ? g_game->GetWorld() : nullptr;
        s32 levelID = deathWorld ? deathWorld->GetCurLevelID() : 0;
        if (levelID == 6) {
            Open();
        }
        else {
            doorState = 1;
            CSoundDirect::PlayTransient(158, static_cast<void*>(&pos), 0, 0);
        }

        DoorCreateUnlockEffects(*this);
    }
}
