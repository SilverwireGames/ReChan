// The pointer-based skeleton build against the copying one it replaced
// (skeleton_reference.h): identical matrices, bit for bit, and identical
// callback inputs and joint state, over random flag mixes.
#include "../../vendor/libp3d/p3d/skeleton.cpp"
#include "skeleton_reference.h"

#include <cstdio>
#include <random>
#include <vector>

static void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "Skeleton regression: %s\n", why); std::abort(); }
}

static std::vector<Mat4> g_seen;

static s32 PreCallback(STreeJoint* joint, u32 index, const Mat4& current) {
    g_seen.push_back(current);
    // Callbacks steer the build through the joint fields, as the game's do.
    joint->rotationY = static_cast<s16>(joint->rotationY + 1000 * static_cast<s32>(index + 1u));
    joint->translationX += 7;
    if (index % 3u == 0u) joint->renderScale = 0.75f;
    if (index % 5u == 1u) {
        joint->useOverrideMatrix = true;
        joint->flags |= STF_MULT_MATRIX;
    }
    return 0;
}

static s32 PostCallback(STreeJoint*, u32, const Mat4& current) {
    g_seen.push_back(current);
    return 0;
}

int main() {
    std::mt19937 rng(20261010u);
    u32 builds = 0;
    for (int round = 0; round < 4000; ++round) {
        const u32 count = 1u + rng() % 40u;
        std::vector<STreeJoint> joints(count);
        for (u32 i = 0; i < count; ++i) {
            STreeJoint& j = joints[i];
            j.flags = 0;
            if (rng() % 8u != 0u) j.flags |= STF_TRANSFORM;
            if (rng() % 4u == 0u) j.flags |= STF_PUSH_MATRIX;
            if (rng() % 4u == 0u) j.flags |= STF_POP_MATRIX;
            if (rng() % 6u == 0u) j.flags |= STF_MULT_MATRIX;
            if (rng() % 6u == 0u) j.flags |= STF_CAPTURE_MATRIX;
            if (rng() % 6u == 0u) j.flags |= STF_LOAD_MATRIX;
            if (rng() % 7u == 0u) { j.flags |= 0x0100u; j.preCallback = PreCallback; }
            if (rng() % 7u == 0u) { j.flags |= 0x1000u; j.postCallback = PostCallback; }
            j.captureBufferIdx = rng() % 5u == 0u ? -1 : static_cast<s32>(rng() % 4u) * 3;
            j.translationX = static_cast<s32>(rng() % 2001u) - 1000;
            j.translationY = static_cast<s32>(rng() % 2001u) - 1000;
            j.translationZ = static_cast<s32>(rng() % 2001u) - 1000;
            j.rotationX = static_cast<s16>(rng());
            j.rotationY = static_cast<s16>(rng());
            j.rotationZ = static_cast<s16>(rng());
            j.useOverrideMatrix = rng() % 2u == 0u;
            for (int e = 0; e < 16; ++e) {
                j.overrideMatrix.m[e] = static_cast<f32>(static_cast<s32>(rng() % 2001u) - 1000) / 333.0f;
            }
            j.renderScale = rng() % 5u == 0u ? 1.25f : 1.0f;
        }
        for (int withCallbacks = 0; withCallbacks < 2; ++withCallbacks) {
            STreeData reference;
            STreeData pointer;
            reference.numJoints = pointer.numJoints = count;
            reference.joints = static_cast<STreeJoint*>(std::malloc(sizeof(STreeJoint) * count));
            pointer.joints = static_cast<STreeJoint*>(std::malloc(sizeof(STreeJoint) * count));
            for (u32 i = 0; i < count; ++i) {
                new (&reference.joints[i]) STreeJoint(joints[i]);
                new (&pointer.joints[i]) STreeJoint(joints[i]);
            }
            std::vector<Mat4> expected(count);
            std::vector<Mat4> actual(count);
            // Rebuild the same skeletons (the rotation cache persists in the
            // joints), changing some angles between builds as animation does.
            for (int pass = 0; pass < 3; ++pass) {
                if (pass > 0) {
                    for (u32 i = 0; i < count; ++i) {
                        if (rng() % 3u != 0u) continue;
                        const s16 angle = static_cast<s16>(rng());
                        const u32 axis = rng() % 3u;
                        s16* a = axis == 0u ? &reference.joints[i].rotationX
                            : axis == 1u ? &reference.joints[i].rotationY : &reference.joints[i].rotationZ;
                        s16* b = axis == 0u ? &pointer.joints[i].rotationX
                            : axis == 1u ? &pointer.joints[i].rotationY : &pointer.joints[i].rotationZ;
                        *a = angle;
                        *b = angle;
                    }
                }
                g_seen.clear();
                ReferenceComputeWorldMatrices(&reference, expected.data(), withCallbacks != 0);
                const std::vector<Mat4> referenceSeen = g_seen;
                g_seen.clear();
                ComputeWorldMatricesInternal(&pointer, actual.data(), withCallbacks != 0);
                require(std::memcmp(expected.data(), actual.data(), sizeof(Mat4) * count) == 0,
                    "matrices differ from the copying build");
                require(referenceSeen.size() == g_seen.size()
                        && (g_seen.empty() || std::memcmp(referenceSeen.data(), g_seen.data(),
                                                          sizeof(Mat4) * g_seen.size()) == 0),
                    "callbacks saw different matrices");
                for (u32 i = 0; i < count; ++i) {
                    const STreeJoint& a = reference.joints[i];
                    const STreeJoint& b = pointer.joints[i];
                    require(a.flags == b.flags && a.rotationX == b.rotationX
                            && a.rotationY == b.rotationY && a.rotationZ == b.rotationZ
                            && a.translationX == b.translationX && a.renderScale == b.renderScale
                            && a.useOverrideMatrix == b.useOverrideMatrix,
                        "joint state differs after the build");
                }
                ++builds;
            }
        }
    }
    std::printf("PASS: skeleton build, %u random builds bit-exact with the copying build\n", builds);
}
