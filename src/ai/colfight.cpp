#include "ai/colfight.h"
#include "ai/humanoid.h"
#include "gen/psxmath_helpers.h"
#include "p3d/p3dmath.h"

// PSX: global array of 12 Humanoid pointers (gArray in decompile)
static Humanoid* g_fightArray[FIGHTING_COLLISION_MAX] = {};
static u32 g_fightGrid[FIGHTING_COLLISION_MAX][FIGHTING_COLLISION_MAX] = {};

static constexpr s32 QUICK_CHECK_MAX_X = 0x800;
static constexpr s32 QUICK_CHECK_MAX_Y = 0x800;
static constexpr s32 QUICK_CHECK_MAX_Z = 0x800;
static constexpr s32 COLFIGHT_MAX_WEAPON_SPACING = 0x80;

// PSX: FindHumanoid__17FightingCollisionPC8Humanoid (COLFIGHT.CPP:116, 0x80072514)
static s32 FindHumanoid(const Humanoid* h) {
    MARKFUNCTION(0x80072514);

    s32 result = -1;
    for (s32 i = 0; i < FIGHTING_COLLISION_MAX; ++i) {
        if (g_fightArray[i] == h) {
            result = i;
        }
    }
    return result;
}

static bool CheckAttackCylinder(
    const Humanoid* target,
    const Humanoid* source,
    const LVector& start,
    const LVector& end,
    s32 radius) {
    MARKFUNCTION(0x80072CE4);

    if (!target || !source) {
        return false;
    }

    const s32 startRelX = start.x - target->pos.x;
    const s32 startRelY = start.y - target->pos.y;
    const s32 startRelZ = start.z - target->pos.z;

    const s32 endRelX = end.x - target->homePos.x;
    const s32 endRelY = end.y - target->homePos.y;
    const s32 endRelZ = end.z - target->homePos.z;

    const s32 deltaX = endRelX - startRelX;
    const s32 deltaY = endRelY - startRelY;
    const s32 deltaZ = endRelZ - startRelZ;

    const s32 minY = target->collBboxMin.y - radius;
    const s32 maxY = target->collBboxMin.z + radius;

    s32 tMin;
    s32 tMax;
    if (deltaY < 0) {
        tMin = rmDiv16i(maxY - startRelY, deltaY);
        tMax = rmDiv16i(minY - startRelY, deltaY);
    }
    else if (deltaY > 0) {
        tMin = rmDiv16i(minY - startRelY, deltaY);
        tMax = rmDiv16i(maxY - startRelY, deltaY);
    }
    else {
        tMin = 0x10000;
        if (startRelY < minY) {
            tMax = 0;
        }
        else {
            tMax = 0;
            if (maxY >= startRelY) {
                tMin = 0;
                tMax = 0x10000;
            }
        }
    }

    if (tMax < tMin || tMin > 0x10000 || tMax < 0) {
        return false;
    }

    const s64 startDot = (s64)startRelX * deltaX + (s64)startRelZ * deltaZ;
    s32 closestT = 0;
    if (startDot < 0) {
        const s64 endDot = (s64)endRelX * deltaX + (s64)endRelZ * deltaZ;
        const s32 negStartDot = static_cast<s32>(-startDot);
        if (endDot > 0) {
            closestT = rmDiv16i(negStartDot, negStartDot + static_cast<s32>(endDot));
        }
        else {
            closestT = 0x10000;
        }
    }

    s32 clampedT = closestT;
    if (closestT >= tMin) {
        if (tMax < closestT) {
            clampedT = tMax;
        }
    }
    else {
        clampedT = tMin;
    }

    const s32 nearestX = startRelX + static_cast<s32>((static_cast<s64>(clampedT) * deltaX) >> 16);
    const s32 nearestZ = startRelZ + static_cast<s32>((static_cast<s64>(clampedT) * deltaZ) >> 16);

    const s32 totalRadius = target->collBboxMax.x + radius;
    return (s64)nearestX * nearestX + (s64)nearestZ * nearestZ
        < (s64)totalRadius * totalRadius;
}

// Weapon sweep step count depends only on the attack (not on the target).
// CheckAttack evaluates it lazily once per call instead of once per target
// (float sqrt + integer division each time); same value, same use.
struct WeaponSweepSteps {
    s32 steps = -1; // -1: not computed yet
};

static bool CheckAttackCylinder(
    const Humanoid* target,
    const Humanoid* source,
    const FightingCollisionAttackType* attackType,
    WeaponSweepSteps* sweepCache = nullptr) {
    MARKFUNCTION(0x800729EC);

    if (!attackType) {
        return false;
    }

    bool hit = CheckAttackCylinder(target, source, attackType->startA, attackType->endA, attackType->radiusA);
    if (!hit && attackType->hasSecondary) {
        const LVector diffStart = {
            attackType->startB.x - attackType->startA.x,
            attackType->startB.y - attackType->startA.y,
            attackType->startB.z - attackType->startA.z,
        };
        const LVector diffEnd = {
            attackType->endB.x - attackType->endA.x,
            attackType->endB.y - attackType->endA.y,
            attackType->endB.z - attackType->endA.z,
        };

        s32 steps;
        if (sweepCache && sweepCache->steps >= 0) {
            steps = sweepCache->steps;
        }
        else {
            const s32 length = static_cast<s32>(rmMag3(
                static_cast<f32>(diffStart.x),
                static_cast<f32>(diffStart.y),
                static_cast<f32>(diffStart.z)));
            steps = (length / COLFIGHT_MAX_WEAPON_SPACING) + 1;
            if (sweepCache && steps >= 0) {
                sweepCache->steps = steps;
            }
        }
        for (s32 i = 1; !hit && i <= steps; ++i) {
            const s32 ratio = i * (0x10000 / steps);
            LVector interpStart = {
                attackType->startA.x + static_cast<s32>((static_cast<s64>(ratio) * diffStart.x) >> 16),
                attackType->startA.y + static_cast<s32>((static_cast<s64>(ratio) * diffStart.y) >> 16),
                attackType->startA.z + static_cast<s32>((static_cast<s64>(ratio) * diffStart.z) >> 16),
            };
            LVector interpEnd = {
                attackType->endA.x + static_cast<s32>((static_cast<s64>(ratio) * diffEnd.x) >> 16),
                attackType->endA.y + static_cast<s32>((static_cast<s64>(ratio) * diffEnd.y) >> 16),
                attackType->endA.z + static_cast<s32>((static_cast<s64>(ratio) * diffEnd.z) >> 16),
            };
            hit = CheckAttackCylinder(target, source, interpStart, interpEnd, attackType->radiusB);
        }
    }

    return hit;
}

static bool CheckAttackPair(
    const Humanoid* target,
    const Humanoid* source,
    const FightingCollisionAttackType* attackType,
    WeaponSweepSteps* sweepCache = nullptr) {
    MARKFUNCTION(0x800729CC);
    return CheckAttackCylinder(target, source, attackType, sweepCache);
}

namespace FightingCollision {

// PSX: Init (COLFIGHT.CPP:159, 0x800725FC)
s32 Init() {
    MARKFUNCTION(0x800725FC);

    for (s32 i = 0; i < FIGHTING_COLLISION_MAX; ++i) {
        g_fightArray[i] = nullptr;
        for (s32 j = 0; j < FIGHTING_COLLISION_MAX; ++j) {
            g_fightGrid[i][j] = 0;
        }
    }

    return 1;
}

// PSX: GetHumanoidArray (COLFIGHT.CPP:258, 0x80072768)
Humanoid** GetHumanoidArray() {
    MARKFUNCTION(0x80072768);
    return g_fightArray;
}

// PSX: InsertHumanoid (COLFIGHT.CPP:181, 0x80072668)
s32 InsertHumanoid(Humanoid* h) {
    MARKFUNCTION(0x80072668);

    if (!h) {
        return 0;
    }

    if (FindHumanoid(h) < 0) {
        const s32 freeIndex = FindHumanoid(nullptr);
        if (freeIndex < 0) {
            return 0;
        }
        g_fightArray[freeIndex] = h;
    }

    return 1;
}

// PSX: RemoveHumanoid (COLFIGHT.CPP:224, 0x800726E0)
s32 RemoveHumanoid(const Humanoid* h) {
    MARKFUNCTION(0x800726E0);

    if (!h) {
        return 0;
    }

    const s32 index = FindHumanoid(h);
    if (index < 0) {
        return 0;
    }

    g_fightArray[index] = nullptr;
    for (s32 i = 0; i < FIGHTING_COLLISION_MAX; ++i) {
        g_fightGrid[index][i] = 0;
        g_fightGrid[i][index] = 0;
    }

    return 1;
}

// PSX: ClearAttack__17FightingCollisionPC8Humanoid (COLFIGHT.CPP:266, 0x80072774)
s32 ClearAttack(const Humanoid* h) {
    MARKFUNCTION(0x80072774);

    const s32 index = FindHumanoid(h);
    if (index < 0) {
        return 0;
    }

    for (s32 i = 0; i < FIGHTING_COLLISION_MAX; ++i) {
        g_fightGrid[index][i] = 0;
    }

    return 1;
}

// PSX: CheckAttack__17FightingCollisionPP8HumanoidiPC8HumanoidPC27FightingCollisionAttackType
// (COLFIGHT.CPP:306, 0x800727D8)
s32 CheckAttack(
    Humanoid** outHumanoids,
    s32 maxCount,
    const Humanoid* source,
    const FightingCollisionAttackType* attackType) {
    MARKFUNCTION(0x800727D8);

    if (!source || !attackType) {
        return 0;
    }

    u32 attackMask = 0;
    if (static_cast<u32>(attackType->attackType) < 4u) {
        attackMask = 1u << static_cast<u32>(attackType->attackType);
    }
    if (!attackMask) {
        return 0;
    }

    const s32 sourceIndex = FindHumanoid(source);
    if (sourceIndex < 0) {
        return 0;
    }

    s32 hitCount = 0;
    WeaponSweepSteps sweepCache;
    for (s32 i = 0; i < FIGHTING_COLLISION_MAX; ++i) {
        Humanoid* target = g_fightArray[i];
        if (!target || target == source) {
            continue;
        }

        if ((g_fightGrid[sourceIndex][i] & attackMask) != 0) {
            continue;
        }

        const s32 dx = PsxAbsS32(target->pos.x - source->pos.x);
        const s32 dy = PsxAbsS32(target->pos.y - source->pos.y);
        const s32 dz = PsxAbsS32(target->pos.z - source->pos.z);
        if (dx < QUICK_CHECK_MAX_X && dy < QUICK_CHECK_MAX_Y && dz < QUICK_CHECK_MAX_Z
            && CheckAttackPair(target, source, attackType, &sweepCache)) {
            if (hitCount < maxCount && outHumanoids) {
                outHumanoids[hitCount] = target;
            }
            ++hitCount;
            g_fightGrid[sourceIndex][i] |= attackMask;
        }
    }

    return hitCount;
}

// PSX: Set__17FightingCollisionPC8HumanoidT1 (COLFIGHT.CPP:849, 0x80072FB4)
s32 Set(const Humanoid* source, const Humanoid* target) {
    MARKFUNCTION(0x80072FB4);

    const s32 sourceIndex = FindHumanoid(source);
    const s32 targetIndex = FindHumanoid(target);
    if (sourceIndex >= 0 && targetIndex >= 0) {
        g_fightGrid[sourceIndex][targetIndex] = 1;
        return 1;
    }

    return 0;
}

} // namespace FightingCollision
