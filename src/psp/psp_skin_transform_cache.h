#pragma once

#include "p3d/matrix.h"
#include <array>
#include <cstdint>
#include <cstring>

// Single render-thread scratch. Begin() must precede every drawable: joint
// matrices are immutable only for that draw, not across instances or frames.
// Direct mapping bounds RAM at 8 KiB; collisions affect speed, never results.
class PspSkinTransformCache {
public:
    void Begin() { valid.fill(0); }

    bool Transform(std::uint16_t source, std::uint32_t joint, const Mat4& matrix,
                   float x, float y, float z, float& ox, float& oy, float& oz) {
        const unsigned slot = source & 255u;
        const unsigned word = slot >> 5;
        const std::uint32_t mask = 1u << (slot & 31u);
        Entry& entry = entries[slot];
        const float local[3] = {x, y, z};
        const bool hit = (valid[word] & mask) && entry.source == source
            && entry.joint == joint
            && std::memcmp(entry.local, local, sizeof(local)) == 0;
        if (!hit) {
            entry.source = source;
            entry.joint = joint;
            std::memcpy(entry.local, local, sizeof(local));
            Mat4TransformPoint(matrix, x, y, z,
                               entry.world[0], entry.world[1], entry.world[2]);
            valid[word] |= mask;
        }
        ox = entry.world[0]; oy = entry.world[1]; oz = entry.world[2];
        return hit;
    }

private:
    struct Entry {
        float local[3];
        float world[3];
        std::uint32_t joint;
        std::uint16_t source;
    };
    std::array<Entry, 256> entries;
    std::array<std::uint32_t, 8> valid{};
};
