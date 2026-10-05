#include "ai/activezn.h"
#include "pc/autotest.h"
#include "ai/behaviour.h"
#include "ai/humanoid.h"
#include "ai/player.h"
#include "ai/thing.h"
#include "gen/ai.h"
#include "gen/database.h"
#include "gen/path.h"
#include "p3d/p3dmath.h"

// SubZoneVolume

// PSX: _13SubZoneVolumeP8DBVolume (ACTIVEZN.CPP:170)
SubZoneVolume::SubZoneVolume(DBVolume* vol) {
    SetName(vol->GetName(), 1);
    box.SetBox(vol);
}

// PSX: IsInSubZoneVolume__13SubZoneVolumeP5Thing (ACTIVEZN.CPP:183)
bool SubZoneVolume::IsInSubZoneVolume(Thing* thing) const {
    return box.IsInside(thing->pos);
}

// ActiveZone

// PSX: _10ActiveZoneP8DBVolumeUl (ACTIVEZN.CPP:143)
ActiveZone::ActiveZone(DBVolume* vol) {
    SetName(vol->GetName(), 1);

    overlordType = 0;
    overlordValue = 0;
    overlordID = 0;
    memberCount = 0;
    members[0] = nullptr;
    members[1] = nullptr;
    members[2] = nullptr;

    // PSX: attrib 6 = overlord type
    const DBAttrib* a6 = vol->FindAttrib(6);
    if (a6) {
        u8 val = (u8)a6->value;
        overlordValue = val;
        if ((u8)(val - 1) < 3) {
            overlordType = 1;
            // PSX: attrib 7 = overlord ID
            const DBAttrib* a7 = vol->FindAttrib(7);
            if (a7)
                overlordID = a7->value;
            else
                overlordID = 0;
        }
        else {
            overlordValue = 0;
        }
    }

    // PSX: attrib 9 = special flag
    specialFlag = 0;
    if (vol->FindAttrib(9))
        specialFlag = 1;

    box.SetBox(vol);
}

// PSX: __10ActiveZone (ACTIVEZN.CPP:380)
ActiveZone::~ActiveZone() {
    // Drain and delete paths
    ccMinNode* n;
    while ((n = pathList.RemHead()) != nullptr) {
        delete n;
    }
    // subZoneList drains via Purge (no delete - ccNode dtor chain)
}

// PSX: AddLinearPath__10ActiveZoneR10LinearPath (ACTIVEZN.CPP:505)
void ActiveZone::AddLinearPath(LinearPath* path) {
    pathList.AddNodeTail(path);
}

// PSX: AddSubZoneVolume__10ActiveZoneR13SubZoneVolume (ACTIVEZN.CPP:512)
void ActiveZone::AddSubZoneVolume(SubZoneVolume* szv) {
    subZoneList.AddNodeTail(szv);
}

// PSX: AddHumanoidToOverlordMembers__10ActiveZoneP8Humanoid (ACTIVEZN.CPP:390)
void ActiveZone::AddHumanoidToOverlordMembers(Humanoid* h) {
    if (AutoTest::TraceSpawns()) {
        LOG_ALWAYS("[Spawn] zone 0x%08X add member %p (count %u)", nameCRC, (void*)h, (u32)memberCount);
    }
    if (memberCount >= 3)
        return;
    memberCount++;
    for (int i = 0; i < 3; i++) {
        if (!members[i]) {
            members[i] = h;
            return;
        }
    }
}

// PSX: RemoveHumanoidFromOverlordMembers__10ActiveZoneP8Humanoid (ACTIVEZN.CPP:417)
void ActiveZone::RemoveHumanoidFromOverlordMembers(Humanoid* h) {
    for (int i = 0; i < 3; i++) {
        if (members[i] == h) {
            if (AutoTest::TraceSpawns()) {
                LOG_ALWAYS("[Spawn] zone 0x%08X remove member %p (count %u)", nameCRC, (void*)h, (u32)memberCount);
            }
            members[i] = nullptr;
            memberCount--;
            return;
        }
    }
}

// PSX: GetNumberOfThinkingMembers__10ActiveZone (ACTIVEZN.CPP:433, 0x800A6E30)
s32 ActiveZone::GetNumberOfThinkingMembers() const {
    MARKFUNCTION(0x800A6E30);

    s32 thinkingCount = 0;
    for (s32 index = 0; index < 3; index++) {
        Humanoid* member = members[index];
        if (member && (member->flags & TF_ACTIVATED) != 0) {
            thinkingCount++;
        }
    }

    return thinkingCount;
}

// PSX: AllowedToMoveIn__10ActiveZoneP8Humanoid (ACTIVEZN.CPP:456, 0x800A6E7C)
s32 ActiveZone::AllowedToMoveIn(Humanoid* humanoid) {
    MARKFUNCTION(0x800A6E7C);

    s32 activeFighters = 0;
    for (s32 index = 0; index < 3; index++) {
        Humanoid* member = members[index];
        if (!member || member == humanoid) {
            continue;
        }

        Behaviour* behaviour = member->behaviour;
        if (behaviour && (u32)(behaviour->ndmsRangeBand - 2) < 2u) {
            activeFighters++;
        }
    }

    if (activeFighters == 0) {
        return 1;
    }

    if (activeFighters >= (s32)overlordValue) {
        return 0;
    }

    return ((s32)rmRangedRandom(100) < overlordID) ? 1 : 0;
}

// PSX: IsInActiveZone__10ActiveZoneP5Thing (ACTIVEZN.CPP:680)
bool ActiveZone::IsInActiveZone(Thing* thing) const {
    return box.IsInside(thing->pos);
}

// PSX: GetActiveZoneCenterPoint__10ActiveZone (ACTIVEZN.CPP:217, 0x800A6B40)
void ActiveZone::GetActiveZoneCenterPoint(LVector& outCenter) const {
    MARKFUNCTION(0x800A6B40);

    // PSX sequence: sum (addu), add sign bit (srl), then arithmetic half (sra).
    const s32 sumX = (s32)((u32)box.minX + (u32)box.maxX);
    const s32 sumY = (s32)((u32)box.minY + (u32)box.maxY);
    const s32 sumZ = (s32)((u32)box.minZ + (u32)box.maxZ);

    const s32 centerX = (sumX + (s32)((u32)sumX >> 31)) >> 1;
    const s32 centerY = (sumY + (s32)((u32)sumY >> 31)) >> 1;
    const s32 centerZ = (sumZ + (s32)((u32)sumZ >> 31)) >> 1;

    outCenter.x = centerX;
    outCenter.y = centerY;
    outCenter.z = centerZ;
}

// PSX: FindFirstValidPath__10ActiveZoneP8Humanoid (ACTIVEZN.CPP:942, 0x800A6F98)
LinearPath* ActiveZone::FindFirstValidPath(Humanoid* humanoid) {
    MARKFUNCTION(0x800A6F98);

    for (ccMinNode* node = pathList.head; node; node = node->next) {
        LinearPath* path = static_cast<LinearPath*>(node);
        if (DoAICheck(path, 0, humanoid) != 0) {
            return path;
        }
    }

    return nullptr;
}

// PSX: DoAICheck__10ActiveZoneP10LinearPathlP8Humanoid (ACTIVEZN.CPP:999, 0x800A7040)
s32 ActiveZone::DoAICheck(LinearPath* path, s32 nodeIndex, Humanoid* humanoid) {
    MARKFUNCTION(0x800A7040);

    if (!path || !path->nodeAttribs || !humanoid || nodeIndex < 0 || nodeIndex >= path->numPoints) {
        return 0;
    }

    NodeAttribs* nodeAttrib = &path->nodeAttribs[nodeIndex];
    s32 pass = 1;

    // Grouped OR sets from PSX:
    // 6/7/8, 9/:/;, </=/>, ?/@/A. Presence of each group is tracked by a flag,
    // then combined with final AND semantics after the main loop.
    s32 any678 = 0;
    s32 pass678 = 0;
    s32 any9ab = 0;
    s32 pass9ab = 0;
    s32 anyLtEqGt = 0;
    s32 passLtEqGt = 0;
    s32 anyQuestionAtA = 0;
    s32 passQuestionAtA = 0;

    // PSX initializes this threshold to 0xBEBEBEBE and lets 'B' overwrite it.
    s32 threshold = (s32)0xBEBEBEBE;

    const LVector& nodePos = path->positions[nodeIndex];
    const LVector& endPos = path->positions[path->numPoints - 1];

    for (s32 index = 0; index < nodeAttrib->count && pass != 0; index++) {
        const s32 id = nodeAttrib->ids[index];
        const s32 value = nodeAttrib->values[index];

        switch (id) {
            case '2':
                pass = (value >= ((s32)rmRangedRandom(100) + 1));
                break;

            case '3':
                pass = (humanoid->DistanceFromPoint(nodePos) < value);
                break;

            case '4':
                pass = (humanoid->DistanceFromPoint(nodePos) >= value);
                break;

            case '5':
            {
                ActiveZone* az = g_ai ? static_cast<ActiveZone*>(g_ai->activeZoneList.FindNodeCRC((u32)value, nullptr)) : nullptr;
                pass = (az != nullptr) && az->IsInActiveZone(Player::s_player);
                break;
            }

            case '6':
            case '7':
            case '8':
            {
                any678 = 1;
                SubZoneVolume* szv = static_cast<SubZoneVolume*>(subZoneList.FindNodeCRC((u32)value, nullptr));
                pass678 |= (szv != nullptr) ? (szv->IsInSubZoneVolume(Player::s_player) ? 1 : 0) : 0;
                break;
            }

            case '9':
            case ':':
            case ';':
            {
                any9ab = 1;
                SubZoneVolume* szv = static_cast<SubZoneVolume*>(subZoneList.FindNodeCRC((u32)value, nullptr));
                pass9ab |= (szv != nullptr) ? (!szv->IsInSubZoneVolume(Player::s_player) ? 1 : 0) : 0;
                break;
            }

            case '<':
            case '=':
            case '>':
            {
                anyLtEqGt = 1;
                SubZoneVolume* szv = static_cast<SubZoneVolume*>(subZoneList.FindNodeCRC((u32)value, nullptr));
                passLtEqGt |= (szv != nullptr) ? (szv->IsInSubZoneVolume(humanoid) ? 1 : 0) : 0;
                break;
            }

            case '?':
            case '@':
            case 'A':
            {
                anyQuestionAtA = 1;
                SubZoneVolume* szv = static_cast<SubZoneVolume*>(subZoneList.FindNodeCRC((u32)value, nullptr));
                passQuestionAtA |= (szv != nullptr) ? (!szv->IsInSubZoneVolume(humanoid) ? 1 : 0) : 0;
                break;
            }

            case 'B':
                threshold = value;
                break;

            case 'C':
            {
                s32 dx = humanoid->pos.x - nodePos.x;
                if (dx < 0) {
                    dx = -dx;
                }
                pass = (dx <= threshold);
                break;
            }

            case 'D':
            {
                s32 dy = humanoid->pos.y - nodePos.y;
                if (dy < 0) {
                    dy = -dy;
                }
                pass = (dy <= threshold);
                break;
            }

            case 'E':
            {
                s32 dz = humanoid->pos.z - nodePos.z;
                if (dz < 0) {
                    dz = -dz;
                }
                pass = (dz <= threshold);
                break;
            }

            case 'G':
            case 'H':
            case 'I':
            {
                pass = 0;
                Thing* thing = g_ai ? g_ai->FindThing((u32)value) : nullptr;
                if (thing) {
                    pass = (thing->DistanceFromPoint(humanoid->pos) >= threshold);
                }
                break;
            }

            case 'T':
            {
                s32 foundNearEnd = 0;
                if (g_ai) {
                    for (ccMinNode* node = g_ai->inactivePickupList.head; node; node = node->next) {
                        Thing* thing = static_cast<Thing*>(node);
                        if (thing->DistanceFromPoint(endPos) < value) {
                            foundNearEnd = 1;
                            break;
                        }
                    }
                }
                pass &= foundNearEnd;
                break;
            }

            case 'U':
                pass = (path->field64 == 0);
                path->field64++;
                break;

            case 'V':
            {
                s32 keep = 1;
                const s32 selfDist = humanoid->DistanceFromPoint(nodePos);

                for (s32 memberIndex = 0; memberIndex < 3 && keep != 0; memberIndex++) {
                    Humanoid* member = members[memberIndex];
                    if (member && member != humanoid) {
                        const s32 memberDist = member->DistanceFromPoint(nodePos);
                        if (memberDist < value && memberDist < selfDist) {
                            keep = 0;
                        }
                    }
                }

                pass &= keep;
                break;
            }

            case '_':
                pass = ((u16)humanoid->health >= (u16)value);
                break;

            case '`':
                pass = ((u16)humanoid->health < (u16)value);
                break;

            case 'a':
                pass = Player::s_player ? ((u16)Player::s_player->health >= (u16)value) : 0;
                break;

            case 'b':
                pass = Player::s_player ? ((u16)Player::s_player->health < (u16)value) : 0;
                break;

            case 'c':
            {
                pass = 0;
                Behaviour* behaviour = humanoid->behaviour;
                if (behaviour && behaviour->handlerDispatch == -1 && behaviour->handler == Behaviour::Idle) {
                    pass = 1;
                }
                break;
            }

            default:
                break;
        }
    }

    if (any678) {
        pass &= pass678;
    }
    if (any9ab) {
        pass &= pass9ab;
    }
    if (anyLtEqGt) {
        pass &= passLtEqGt;
    }
    if (anyQuestionAtA) {
        pass &= passQuestionAtA;
    }

    return pass;
}

// PSX: DoActionsAtNode__10ActiveZoneP10LinearPathlP8Humanoid (ACTIVEZN.CPP:1378, 0x800A7784)
s32 ActiveZone::DoActionsAtNode(LinearPath* path, s32 nodeIndex, Humanoid* humanoid) {
    MARKFUNCTION(0x800A7784);

    if (!path || !path->nodeAttribs || !humanoid || nodeIndex < 0 || nodeIndex >= path->numPoints) {
        return 0;
    }

    NodeAttribs* nodeAttrib = &path->nodeAttribs[nodeIndex];

    s32 firstActionIndex = -1;
    for (s32 index = 0; index < nodeAttrib->count; index++) {
        if (nodeAttrib->ids[index] >= 'd') {
            firstActionIndex = index;
            break;
        }
    }

    if (firstActionIndex < 0) {
        return 0;
    }

    s32 valueIndex = firstActionIndex;
    for (s32 actionIndex = firstActionIndex;
         actionIndex < nodeAttrib->count && nodeAttrib->ids[actionIndex] < 's';
         actionIndex++, valueIndex++) {
        const s32 actionID = nodeAttrib->ids[actionIndex];

        switch (actionID) {
            case 'd':
                humanoid->RequestAction(2);
                break;

            case 'e':
                humanoid->RequestAction(3);
                break;

            case 'f':
                humanoid->RequestAction(9);
                break;

            case 'g':
                humanoid->RequestAction(8);
                break;

            case 'h':
                humanoid->SetActionState(AS_STAND, 0);
                humanoid->RequestAction(7);
                break;

            case 'i':
                humanoid->RequestAction(7);
                break;

            case 'j':
                humanoid->field316 = nodeAttrib->GetAttrib('j');
                humanoid->SetTauntAnim(nodeAttrib->GetAttrib('o'));
                humanoid->SetActionState(AS_TAUNT_PAUSE, 0);
                break;

            case 'k':
            {
                LinearPath* nextPath = static_cast<LinearPath*>(pathList.FindNodeCRC((u32)nodeAttrib->values[valueIndex], nullptr));
                if (nextPath && humanoid->behaviour) {
                    humanoid->behaviour->InitPathAIState(nextPath);
                }
                break;
            }

            case 'l':
            {
                LinearPath* nextPath = static_cast<LinearPath*>(pathList.FindNodeCRC((u32)nodeAttrib->values[valueIndex], nullptr));
                if (nextPath && DoAICheck(nextPath, 0, humanoid) != 0 && humanoid->behaviour) {
                    humanoid->behaviour->InitPathAIState(nextPath);
                }
                break;
            }

            case 'm':
                humanoid->SetTarget(Player::s_player);
                break;

            case 'n':
                break;

            case 'o':
                humanoid->SetTauntAnim(nodeAttrib->GetAttrib('o'));
                if (humanoid->actionState != AS_TAUNT_PAUSE) {
                    humanoid->FaceThingDesired(Player::s_player);
                    humanoid->SetActionState(AS_TAUNT_ENTRY, 0);
                }
                break;

            case 'p':
                humanoid->RequestAction(7);
                break;

            case 'q':
                humanoid->SetActionState(AS_STAND, 0);
                humanoid->RequestAction(5);
                break;

            case 'r':
                humanoid->Kill();
                break;

            default:
                break;
        }
    }

    return 0;
}

// PSX: IsPathNodeTerminator__10ActiveZoneP10LinearPathl (ACTIVEZN.CPP:1318)
bool ActiveZone::IsPathNodeTerminator(LinearPath* path, s32 nodeIndex) const {
    MARKFUNCTION(0x800A76AC);

    if (!path || !path->nodeAttribs || nodeIndex < 0 || nodeIndex >= path->numPoints) {
        return false;
    }

    const NodeAttribs& attribs = path->nodeAttribs[nodeIndex];
    for (s32 index = 0; index < attribs.count; index++) {
        if (attribs.ids[index] == 'W') {
            return true;
        }
    }

    return false;
}

// PSX: AllowBreakoffOfDestinationNode__10ActiveZoneP10LinearPathl (ACTIVEZN.CPP:1352)
bool ActiveZone::AllowBreakoffOfDestinationNode(LinearPath* path, s32 nodeIndex) const {
    MARKFUNCTION(0x800A7718);

    if (!path || !path->nodeAttribs || nodeIndex < 0 || nodeIndex >= path->numPoints) {
        return false;
    }

    const NodeAttribs& attribs = path->nodeAttribs[nodeIndex];
    for (s32 index = 0; index < attribs.count; index++) {
        if (attribs.ids[index] == 'X') {
            return true;
        }
    }

    return false;
}
