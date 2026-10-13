#pragma once
#include "ai/obstacle.h"

extern const tagCollisionBox INVALID_COLLISION_BOX;

bool ObstacleFillCollisionBox(tagCollisionBox& box, const DBRoot* root, u32 attribNum);
void ApplyDoorStandingZExtent(tagCollisionBox& box);

bool CorrectThingPositionObstacle(
    const LVector& basisA,
    const LVector& basisB,
    s32 rotA,
    s32 rotB,
    const tagCollisionBox& box,
    const LVector& pointA,
    const LVector& pointB,
    s32 radius,
    s32 yMinOffset,
    s32 yMaxOffset,
    LVector& outPos,
    LVector& outNormal,
    LVector& outPushedPos);

void SetCorrectThingPositionRadiusBias(s32 value);

void ObstacleBuildRenderTransform(
    const Thing* owner,
    const LVector& logicPos,
    const LVector& logicOrientation,
    LVector& outPos,
    LVector& outOrientation);

void ObstacleForgetRenderTransform(const Thing* owner);

static inline s32 MulShift16(s32 a, s32 b) {
    return static_cast<s32>((static_cast<s64>(a) * b) >> 16);
}

inline s32 Div2TowardZero(s32 value) {
    return (value + (s32)((u32)value >> 31)) >> 1;
}