#pragma once

#include "p3d/matrix.h"

#include <cmath>
#include <cstdint>

// Conservative whole-character view rejection for CPU-skinned models.
//
// A skinned vertex is joints[j] * local with |local| <= radius[j]; its distance
// from the joint origin is bounded by the Frobenius norm of the joint's 3x3
// (Frobenius >= spectral norm) times that radius. A character is rejected only
// when every joint sphere lies entirely outside one of the four side planes of
// clip = projection * view * world, i.e. no vertex can satisfy |x|,|y| <= w.
// Near/far are not tested; NaN, non-finite or degenerate input keeps the draw.
namespace PspSkinViewCull {

constexpr std::uint32_t kMaxJoints = 256u;

// Number of joint slots referenced by the vertices, or 0 when any joint index
// is out of range (the caller then never culls).
template <typename Vertex>
inline std::uint32_t JointSlotCount(const Vertex* verts, std::uint32_t vertexCount) {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < vertexCount; ++i) {
        const std::uint32_t joint = verts[i].jointIdx;
        if (joint >= kMaxJoints) {
            return 0;
        }
        if (joint + 1u > count) {
            count = joint + 1u;
        }
    }
    return count;
}

// Fills radii[0..slotCount) with the largest joint-local distance (-1 for a
// joint without vertices). Returns false for non-finite input.
template <typename Vertex>
inline bool FillJointRadii(const Vertex* verts, std::uint32_t vertexCount,
                           f32* radii, std::uint32_t slotCount) {
    for (std::uint32_t joint = 0; joint < slotCount; ++joint) {
        radii[joint] = -1.0f;
    }
    for (std::uint32_t i = 0; i < vertexCount; ++i) {
        const Vertex& v = verts[i];
        const f32 radius = std::sqrt(v.lx * v.lx + v.ly * v.ly + v.lz * v.lz);
        if (!std::isfinite(radius) || v.jointIdx >= slotCount) {
            return false;
        }
        if (radius > radii[v.jointIdx]) {
            radii[v.jointIdx] = radius;
        }
    }
    return true;
}

inline bool OutsideView(const Mat4& clip, const f32* radii, std::uint32_t slotCount,
                        const Mat4* joints, std::uint32_t jointCount) {
    if (!radii || !joints || slotCount == 0 || slotCount > jointCount) {
        return false;
    }

    // Column-major: row r of clip is (m[r], m[4 + r], m[8 + r], m[12 + r]).
    // Planes: w + x, w - x, w + y, w - y >= 0.
    f32 planes[4][4];
    for (std::uint32_t plane = 0; plane < 4u; ++plane) {
        const std::uint32_t axis = plane >> 1;
        const f32 sign = (plane & 1u) ? -1.0f : 1.0f;
        for (std::uint32_t column = 0; column < 4u; ++column) {
            planes[plane][column] = clip.m[column * 4u + 3u]
                + sign * clip.m[column * 4u + axis];
        }
        const f32 length = std::sqrt(planes[plane][0] * planes[plane][0]
            + planes[plane][1] * planes[plane][1]
            + planes[plane][2] * planes[plane][2]);
        if (!std::isfinite(length) || !(length > 1.0e-12f)) {
            return false;
        }
        const f32 inverse = 1.0f / length;
        for (std::uint32_t column = 0; column < 4u; ++column) {
            planes[plane][column] *= inverse;
        }
    }

    for (std::uint32_t joint = 0; joint < slotCount; ++joint) {
        const f32 localRadius = radii[joint];
        if (localRadius < 0.0f) {
            continue;
        }
        const f32* m = joints[joint].m;
        const f32 frobenius = std::sqrt(
            m[0] * m[0] + m[1] * m[1] + m[2] * m[2]
            + m[4] * m[4] + m[5] * m[5] + m[6] * m[6]
            + m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
        // Relative and absolute slack absorb float rounding.
        const f32 radius = localRadius * frobenius * 1.01f + 1.0f;
        bool outside = false;
        for (std::uint32_t plane = 0; plane < 4u; ++plane) {
            const f32 distance = planes[plane][0] * m[12] + planes[plane][1] * m[13]
                + planes[plane][2] * m[14] + planes[plane][3];
            // NaN compares false and therefore keeps the character.
            if (distance < -radius) {
                outside = true;
                break;
            }
        }
        if (!outside) {
            return false;
        }
    }
    return true;
}

} // namespace PspSkinViewCull
