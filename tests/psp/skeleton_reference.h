// The skeleton build as of commit 9ba548b (a Mat4 copy per push, pop,
// product and output), kept verbatim as the bit-exact reference for the
// pointer-based build. Included by skeleton_build.cpp after skeleton.cpp.
#pragma once

static void ReferenceComputeWorldMatrices(STreeData* skeleton, Mat4* outMatrices, bool invokeCallbacks) {
    if (!skeleton || !skeleton->joints || skeleton->numJoints == 0) {
        return;
    }
#if defined(RC_PLATFORM_PSP)
    PspPerf::ScopeTimer skeletonTimer(PspPerf::RendererCounter::SkeletonUs);
    PspPerf::CountRenderer(PspPerf::RendererCounter::SkeletonBuilds);
    PspPerf::CountRenderer(PspPerf::RendererCounter::SkeletonJoints, skeleton->numJoints);
#endif

    UninitMat4 matStack[32];
    s32 stackTop = 0;
    Mat4 current;

    // Capture/load buffer keys are the raw dword offsets loaded from the joint chunk.
    s32 captureKeys[64];
    UninitMat4 captureValues[64];
    bool captureValid[64];
    s32 captureCount = 0;

    auto findCaptureSlot = [&](s32 key) -> s32 {
        for (s32 i = 0; i < captureCount; i++) {
            if (captureKeys[i] == key) {
                return i;
            }
        }
        if (captureCount >= 64) {
            return -1;
        }
        captureKeys[captureCount] = key;
        captureValid[captureCount] = false;
        return captureCount++;
    };

    for (u32 i = 0; i < skeleton->numJoints; i++) {
        STreeJoint& j = skeleton->joints[i];
        const bool preserveJointState = invokeCallbacks
            && ((j.flags & (STF_PRE_CALLBACK_MASK | STF_POST_CALLBACK_MASK)) != 0);

        u32 savedFlags = 0;
        s32 savedTranslationX = 0;
        s32 savedTranslationY = 0;
        s32 savedTranslationZ = 0;
        s16 savedRotationX = 0;
        s16 savedRotationY = 0;
        s16 savedRotationZ = 0;
        bool savedUseOverrideMatrix = false;
        UninitMat4 savedOverrideMatrix;
        f32 savedRenderScale = 1.0f;

        if (preserveJointState) {
            savedFlags = j.flags;
            savedTranslationX = j.translationX;
            savedTranslationY = j.translationY;
            savedTranslationZ = j.translationZ;
            savedRotationX = j.rotationX;
            savedRotationY = j.rotationY;
            savedRotationZ = j.rotationZ;
            savedUseOverrideMatrix = j.useOverrideMatrix;
            savedOverrideMatrix.value = j.overrideMatrix;
            savedRenderScale = j.renderScale;
        }

        if (j.flags & STF_PUSH_MATRIX) {
            if (stackTop < 32) {
                matStack[stackTop++].value = current;
            }
        }

        if (invokeCallbacks && (j.flags & STF_PRE_CALLBACK_MASK) && j.preCallback) {
#if defined(RC_PLATFORM_PSP)
            PspPerf::ScopeTimer callbackTimer(PspPerf::RendererCounter::SkelCallbackUs);
#endif
            j.preCallback(&j, i, current);
        }

        if ((j.flags & STF_MULT_MATRIX) && j.useOverrideMatrix) {
            current = current * j.overrideMatrix;
        }

        if (j.flags & STF_TRANSFORM) {
            Mat4 local;
            p3dBuildRotMatrixYZX(j.rotationX, j.rotationY, j.rotationZ, local);
            local.SetTranslation((f32)j.translationX, (f32)j.translationY, (f32)j.translationZ);
            current = current * local;
        }

        if (j.renderScale != 1.0f) {
            current.ScaleRotation(j.renderScale);
        }

        if (invokeCallbacks && (j.flags & STF_POST_CALLBACK_MASK) && j.postCallback) {
#if defined(RC_PLATFORM_PSP)
            PspPerf::ScopeTimer callbackTimer(PspPerf::RendererCounter::SkelCallbackUs);
#endif
            j.postCallback(&j, i, current);
        }

        if (j.flags & STF_CAPTURE_MATRIX) {
            if (j.captureBufferIdx >= 0) {
                s32 slot = findCaptureSlot(j.captureBufferIdx);
                if (slot >= 0) {
                    captureValues[slot].value = current;
                    captureValid[slot] = true;
                }
            }
        }

        if (j.flags & STF_LOAD_MATRIX) {
            if (j.captureBufferIdx >= 0) {
                s32 slot = findCaptureSlot(j.captureBufferIdx);
                if (slot >= 0 && captureValid[slot]) {
                    current = captureValues[slot].value;
                }
            }
        }

        outMatrices[i] = current;

        if (j.flags & STF_POP_MATRIX) {
            if (stackTop > 0) {
                current = matStack[--stackTop].value;
            }
        }

        if (preserveJointState) {
            // PSX callbacks affect the matrix build for this draw only.
            j.flags = savedFlags;
            j.translationX = savedTranslationX;
            j.translationY = savedTranslationY;
            j.translationZ = savedTranslationZ;
            j.rotationX = savedRotationX;
            j.rotationY = savedRotationY;
            j.rotationZ = savedRotationZ;
            j.useOverrideMatrix = savedUseOverrideMatrix;
            j.overrideMatrix = savedOverrideMatrix.value;
            j.renderScale = savedRenderScale;
        }
    }
}
