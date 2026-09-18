#pragma once

#include "core.h"
#include "p3d/chunkfile.h"
#include "p3d/camera.h"
#include "p3d/lvector.h"
#include <algorithm>
#include <utility>
#include <vector>

constexpr u32 CAMERA_PARAM_ANIM_MAGIC = 0x50414D43; // "CMAP"

enum class CameraParamKeyKind : u8 {
    None = 0,
    Static1D,
    Static3D,
    Angle1D,
};

struct CameraParamKey {
    s32 paramId = 0;
    s32 outputIndex = 0;
    CameraParamKeyKind kind = CameraParamKeyKind::None;
    std::vector<u16> times;
    std::vector<s32> scalarValues;
    std::vector<LVector> vectorValues;
    std::vector<u16> angleValues;
};

struct CameraParamClip {
    s32 startFrame = 0;
    s32 numFrames = 0;
    std::vector<CameraParamKey> keys;
};

// The first two fields intentionally mirror TransformAnim so AnimStructure can
// reuse numFrames for camera playback timing.
struct CameraParamAnim {
    u32 nameUID = 0;
    s32 numFrames = 0;
    s32 clipCount = 0;
    s32 reserved = 0;
    u32 magic = CAMERA_PARAM_ANIM_MAGIC;
    std::vector<CameraParamClip> clips;
};

inline bool IsCameraParamAnim(const void* anim) {
    if (!anim) {
        return false;
    }

    const CameraParamAnim* cameraAnim = reinterpret_cast<const CameraParamAnim*>(anim);
    return cameraAnim->magic == CAMERA_PARAM_ANIM_MAGIC;
}

inline s32 ReadParamAnimS32(const u8* data) {
    return static_cast<s32>(
        static_cast<u32>(data[0]) |
        (static_cast<u32>(data[1]) << 8) |
        (static_cast<u32>(data[2]) << 16) |
        (static_cast<u32>(data[3]) << 24));
}

inline u32 ReadParamAnimU32(const u8* data) {
    return static_cast<u32>(ReadParamAnimS32(data));
}

inline bool ParseCameraParamKey(tChunkFile& file, CameraParamKey& key) {
    file.GetPString();
    file.GetUInt();
    key.paramId = file.GetInt();

    constexpr u16 CHUNK_PARAM_TIMES = 0x4203;
    constexpr u16 CHUNK_PARAM_STATIC_1DOF = 0x4283;
    constexpr u16 CHUNK_PARAM_STATIC_3DOF = 0x4285;
    constexpr u16 CHUNK_PARAM_ANGLE_1DOF = 0x4213;

    while (file.ChunksRemaining()) {
        const u16 chunkId = file.BeginChunk();

        switch (chunkId) {
            case CHUNK_PARAM_TIMES: {
                const s32 count = file.GetInt();
                if (count > 0) {
                    key.times.resize(static_cast<size_t>(count));
                    for (s32 index = 0; index < count; index++) {
                        key.times[static_cast<size_t>(index)] = file.GetUShort();
                    }
                }
                break;
            }

            case CHUNK_PARAM_STATIC_1DOF: {
                const s32 count = file.GetInt();
                key.outputIndex = file.GetInt();
                key.kind = CameraParamKeyKind::Static1D;
                if (count > 0) {
                    key.scalarValues.resize(static_cast<size_t>(count));
                    for (s32 index = 0; index < count; index++) {
                        key.scalarValues[static_cast<size_t>(index)] = file.GetInt();
                    }
                }
                break;
            }

            case CHUNK_PARAM_STATIC_3DOF: {
                const s32 count = file.GetInt();
                key.kind = CameraParamKeyKind::Static3D;
                if (count > 0) {
                    key.vectorValues.resize(static_cast<size_t>(count));
                    for (s32 index = 0; index < count; index++) {
                        LVector value = {};
                        value.x = file.GetInt();
                        value.y = file.GetInt();
                        value.z = file.GetInt();
                        key.vectorValues[static_cast<size_t>(index)] = value;
                    }
                }
                break;
            }

            case CHUNK_PARAM_ANGLE_1DOF: {
                const s32 count = file.GetInt();
                key.outputIndex = file.GetInt();
                key.kind = CameraParamKeyKind::Angle1D;
                if (count > 0) {
                    key.angleValues.resize(static_cast<size_t>(count));
                    for (s32 index = 0; index < count; index++) {
                        key.angleValues[static_cast<size_t>(index)] = file.GetUShort();
                    }
                }
                break;
            }

            default:
                break;
        }

        file.EndChunk();
    }

    return !key.times.empty() && key.kind != CameraParamKeyKind::None;
}

inline bool ParseCameraParamClip(tChunkFile& file, CameraParamClip& clip) {
    file.GetPString();
    file.GetUInt();
    file.GetUInt();
    file.GetPString();
    clip.numFrames = file.GetInt();
    const s32 expectedParams = file.GetInt();
    (void)expectedParams;

    constexpr u16 CHUNK_PARAM = 0x4301;

    while (file.ChunksRemaining()) {
        const u16 chunkId = file.BeginChunk();
        if (chunkId == CHUNK_PARAM) {
            CameraParamKey key = {};
            if (ParseCameraParamKey(file, key)) {
                clip.keys.push_back(std::move(key));
            }
        }
        file.EndChunk();
    }

    return clip.numFrames > 0 && !clip.keys.empty();
}

inline void CollectCameraParamClips(tChunkFile& file, std::vector<CameraParamClip>& clips) {
    constexpr u16 CHUNK_P3D_CONTAINER = 0x6000;
    constexpr u16 CHUNK_TEXTURE_PAGE = 0xFF04;
    constexpr u16 CHUNK_PARAM_ANIM = 0x4300;

    while (file.ChunksRemaining()) {
        const u16 chunkId = file.BeginChunk();

        if (chunkId == CHUNK_PARAM_ANIM) {
            CameraParamClip clip = {};
            if (ParseCameraParamClip(file, clip)) {
                clips.push_back(std::move(clip));
            }
        }
        else if (chunkId == CHUNK_P3D_CONTAINER || chunkId == CHUNK_TEXTURE_PAGE) {
            CollectCameraParamClips(file, clips);
        }

        file.EndChunk();
    }
}

inline CameraParamAnim* ParseCameraParamAnim(const u8* animData, u32 animSize, const u8* p3dData, u32 p3dSize) {
    if (!p3dData || p3dSize < 6) {
        return nullptr;
    }

    std::vector<CameraParamClip> clips;
    tChunkFile file(p3dData, p3dSize);
    CollectCameraParamClips(file, clips);
    if (clips.empty()) {
        return nullptr;
    }

    CameraParamAnim* anim = new CameraParamAnim();
    if (animData && animSize >= 4) {
        anim->nameUID = ReadParamAnimU32(animData + 0);
    }

    s32 totalFrames = 0;
    if (animData && animSize >= 16) {
        totalFrames = ReadParamAnimS32(animData + 12);
    }
    if (totalFrames <= 0) {
        for (size_t index = 0; index < clips.size(); index++) {
            if (index == 0) {
                totalFrames = clips[index].numFrames;
            }
            else {
                totalFrames += clips[index].numFrames - 1;
            }
        }
    }

    s32 startFrame = 0;
    for (size_t index = 0; index < clips.size(); index++) {
        clips[index].startFrame = startFrame;
        if (index + 1 < clips.size()) {
            startFrame += clips[index].numFrames - 1;
        }
    }

    anim->numFrames = totalFrames;
    anim->clipCount = static_cast<s32>(clips.size());
    anim->clips = std::move(clips);
    return anim;
}

inline const CameraParamClip* FindCameraParamClip(const CameraParamAnim* anim, s32 frameReal, s32& localFrameReal) {
    if (!anim || anim->clips.empty()) {
        return nullptr;
    }

    const CameraParamClip* clip = &anim->clips.front();
    for (size_t index = 0; index < anim->clips.size(); index++) {
        const CameraParamClip& candidate = anim->clips[index];
        if (frameReal >= (candidate.startFrame << 16)) {
            clip = &candidate;
        }
        else {
            break;
        }
    }

    localFrameReal = frameReal - (clip->startFrame << 16);
    const s32 maxFrameReal = std::max<s32>(0, static_cast<s32>((clip->numFrames - 1) << 16));
    localFrameReal = std::clamp(localFrameReal, static_cast<s32>(0), maxFrameReal);
    return clip;
}

inline constexpr u16 CAMERA_PARAM_OO_TABLE[64] = {
    0, 65535, 32768, 21845, 16384, 13107, 10922, 9362,
    8192, 7281, 6553, 5957, 5461, 5041, 4681, 4369,
    4096, 3855, 3640, 3449, 3276, 3120, 2978, 2849,
    2730, 2621, 2520, 2427, 2340, 2259, 2184, 2114,
    2048, 1985, 1927, 1872, 1820, 1771, 1724, 1680,
    1638, 1598, 1560, 1524, 1489, 1456, 1424, 1394,
    1365, 1337, 1310, 1285, 1260, 1236, 1213, 1191,
    1170, 1149, 1129, 1110, 1092, 1074, 1057, 1040,
};

inline s32 FindCameraParamKeyIndex(const std::vector<u16>& times, s32 frameReal) {
    if (times.empty()) {
        return -1;
    }
    if (times.size() == 1 || frameReal <= (static_cast<s32>(times.front()) << 16)) {
        return 0;
    }

    for (size_t index = 0; index + 1 < times.size(); index++) {
        if (frameReal <= (static_cast<s32>(times[index + 1]) << 16)) {
            return static_cast<s32>(index);
        }
    }

    return static_cast<s32>(times.size() - 1);
}

inline s32 EvaluateCameraParamScalar(const CameraParamKey& key, s32 frameReal) {
    if (key.scalarValues.empty()) {
        return 0;
    }
    if (key.scalarValues.size() == 1 || key.times.empty()) {
        return key.scalarValues.front();
    }

    const s32 index = FindCameraParamKeyIndex(key.times, frameReal);
    if (index < 0 || static_cast<size_t>(index) >= key.scalarValues.size()) {
        return key.scalarValues.back();
    }
    if (static_cast<size_t>(index + 1) >= key.scalarValues.size() || static_cast<size_t>(index + 1) >= key.times.size()) {
        return key.scalarValues[static_cast<size_t>(index)];
    }

    const s32 t0 = static_cast<s32>(key.times[static_cast<size_t>(index)]) << 16;
    const s32 t1 = static_cast<s32>(key.times[static_cast<size_t>(index + 1)]) << 16;
    const s32 v0 = key.scalarValues[static_cast<size_t>(index)];
    const s32 v1 = key.scalarValues[static_cast<size_t>(index + 1)];

    if (frameReal <= t0) {
        return v0;
    }
    if (frameReal >= t1) {
        return v1;
    }

    const s32 deltaFrames = static_cast<s32>(key.times[static_cast<size_t>(index + 1)])
        - static_cast<s32>(key.times[static_cast<size_t>(index)]);
    const u16 ease = (deltaFrames >= 0 && deltaFrames < 64) ? CAMERA_PARAM_OO_TABLE[deltaFrames] : 0;
    const s32 blend = static_cast<s32>((static_cast<s64>(ease) * static_cast<s64>(frameReal - t0)) >> 16);
    return v0 + static_cast<s32>((static_cast<s64>(blend) * static_cast<s64>(v1 - v0)) >> 16);
}

inline LVector EvaluateCameraParamVector(const CameraParamKey& key, s32 frameReal) {
    if (key.vectorValues.empty()) {
        return {};
    }
    if (key.vectorValues.size() == 1 || key.times.empty()) {
        return key.vectorValues.front();
    }

    const s32 index = FindCameraParamKeyIndex(key.times, frameReal);
    if (index < 0 || static_cast<size_t>(index) >= key.vectorValues.size()) {
        return key.vectorValues.back();
    }
    if (static_cast<size_t>(index + 1) >= key.vectorValues.size() || static_cast<size_t>(index + 1) >= key.times.size()) {
        return key.vectorValues[static_cast<size_t>(index)];
    }

    const s32 t0 = static_cast<s32>(key.times[static_cast<size_t>(index)]) << 16;
    const s32 t1 = static_cast<s32>(key.times[static_cast<size_t>(index + 1)]) << 16;
    const LVector& v0 = key.vectorValues[static_cast<size_t>(index)];
    const LVector& v1 = key.vectorValues[static_cast<size_t>(index + 1)];

    if (frameReal <= t0) {
        return v0;
    }
    if (frameReal >= t1) {
        return v1;
    }

    const s32 deltaFrames = static_cast<s32>(key.times[static_cast<size_t>(index + 1)])
        - static_cast<s32>(key.times[static_cast<size_t>(index)]);
    const u16 ease = (deltaFrames >= 0 && deltaFrames < 64) ? CAMERA_PARAM_OO_TABLE[deltaFrames] : 0;
    const s32 blend = static_cast<s32>((static_cast<s64>(ease) * static_cast<s64>(frameReal - t0)) >> 16);

    LVector result = {};
    result.x = v0.x + static_cast<s32>((static_cast<s64>(blend) * static_cast<s64>(v1.x - v0.x)) >> 16);
    result.y = v0.y + static_cast<s32>((static_cast<s64>(blend) * static_cast<s64>(v1.y - v0.y)) >> 16);
    result.z = v0.z + static_cast<s32>((static_cast<s64>(blend) * static_cast<s64>(v1.z - v0.z)) >> 16);
    return result;
}

inline u16 EvaluateCameraParamAngle(const CameraParamKey& key, s32 frameReal) {
    if (key.angleValues.empty()) {
        return 0;
    }
    if (key.angleValues.size() == 1 || key.times.empty()) {
        return key.angleValues.front();
    }

    const s32 index = FindCameraParamKeyIndex(key.times, frameReal);
    if (index < 0 || static_cast<size_t>(index) >= key.angleValues.size()) {
        return key.angleValues.back();
    }
    if (static_cast<size_t>(index + 1) >= key.angleValues.size() || static_cast<size_t>(index + 1) >= key.times.size()) {
        return key.angleValues[static_cast<size_t>(index)];
    }

    const s32 t0 = static_cast<s32>(key.times[static_cast<size_t>(index)]) << 16;
    const s32 t1 = static_cast<s32>(key.times[static_cast<size_t>(index + 1)]) << 16;
    const u16 v0 = key.angleValues[static_cast<size_t>(index)];
    const u16 v1 = key.angleValues[static_cast<size_t>(index + 1)];

    if (frameReal <= t0) {
        return v0;
    }
    if (frameReal >= t1) {
        return v1;
    }

    const s32 deltaFrames = static_cast<s32>(key.times[static_cast<size_t>(index + 1)])
        - static_cast<s32>(key.times[static_cast<size_t>(index)]);
    const u16 ease = (deltaFrames >= 0 && deltaFrames < 64) ? CAMERA_PARAM_OO_TABLE[deltaFrames] : 0;
    const s32 blend = static_cast<s32>((static_cast<s64>(ease) * static_cast<s64>(frameReal - t0)) >> 16);
    const s32 diff = static_cast<s16>(v1 - v0);
    return static_cast<u16>((static_cast<s32>(v0) + static_cast<s32>((static_cast<s64>(blend) * diff) >> 16)) & 0xFFFF);
}

inline bool EvaluateCameraParamAnim(const CameraParamAnim* anim, s32 frameReal, t2PointMatrixCamera* camera) {
    if (!anim || !camera) {
        return false;
    }

    s32 localFrameReal = frameReal;
    const CameraParamClip* clip = FindCameraParamClip(anim, frameReal, localFrameReal);
    if (!clip) {
        return false;
    }

    LVector position = {};
    LVector target = {};
    camera->GetPosition(&position);
    camera->GetTarget(&target);
    u16 twist = camera->GetTwist();
    s32 fovA = 0;
    s32 fovB = 0;
    camera->GetFOV(&fovA, &fovB);

    bool updated = false;
    for (const CameraParamKey& key : clip->keys) {
        switch (key.paramId) {
            case 2:
                position = EvaluateCameraParamVector(key, localFrameReal);
                updated = true;
                break;

            case 17:
                fovA = EvaluateCameraParamScalar(key, localFrameReal);
                updated = true;
                break;

            case 18:
                fovB = EvaluateCameraParamScalar(key, localFrameReal);
                updated = true;
                break;

            case 33:
                target = EvaluateCameraParamVector(key, localFrameReal);
                updated = true;
                break;

            case 34:
                twist = EvaluateCameraParamAngle(key, localFrameReal);
                updated = true;
                break;

            default:
                break;
        }
    }

    if (!updated) {
        return false;
    }

    camera->SetPosition(&position);
    camera->SetTarget(&target);
    camera->SetTwist(twist);
    camera->SetFOV(fovA, fovB);
    return true;
}
