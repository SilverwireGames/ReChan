#pragma once

// Allegrex scratchpad: 16 KB of on-chip SRAM at 0x00010000, user accessible,
// 1-cycle loads/stores that bypass the 16 KB data cache (no line fills, no
// write-backs, no evictions of other data). -DPSP_DISABLE_SCRATCHPAD turns
// every user back to its RAM data for A/B.
//
// Allocation is static and explicit: one fixed region per subsystem, laid
// out below, never overlapping. There is no general allocator.
//
//   offset  size   region       owner / lifetime
//   0x0000  0x040  header       magic + per-region tags (validity, below)
//   0x0040  0x1000 normals      psp_vfpu_skin.h: copy of kNormals (256 x 16 B),
//                               persistent, read-only once filled
//   0x1040  0x0C00 joints       model.cpp DrawableSTree::Display: world matrix
//                               per joint (<= 48 x 64 B), one character draw;
//                               leased (busy flag), larger skeletons use RAM
//   0x1C40  0x2000 collision    colsect.cpp: resident copies of compact sector
//                               boxes (tag + 8 B boxes), persistent cache
//   0x3C40  0x03C0 (free)       unused, kept free
//   total   0x4000 (16384 B)
//
// Threading: every region belongs to the main game/render thread (collision
// queries and character draws). The audio threads and callbacks must never
// touch the scratchpad: it is neither saved per thread nor locked.
//
// Validity: nothing documents the scratchpad as preserved across a system
// suspend/resume, and its power-on contents are arbitrary. Persistent regions
// therefore keep a tag in the header that is checked on every acquire; a
// missing magic invalidates every region at once and each one refills itself.
// The leased joint region is written before it is read on every draw.
//
// Host builds (tests) have no scratchpad: Base() is a plain static buffer, so
// the same region, tag and refill logic runs and is covered by the tests.

#include <cstddef>
#include <cstdint>

#if defined(RC_PLATFORM_PSP) && !defined(PSP_DISABLE_SCRATCHPAD)
#define RC_PSP_SCRATCHPAD 1
#endif

namespace PspScratchpad {

constexpr std::uint32_t kSize = 0x4000;

constexpr std::uint32_t kHeaderOffset = 0x0000;
constexpr std::uint32_t kHeaderSize = 0x0040;
constexpr std::uint32_t kNormalsOffset = 0x0040;
constexpr std::uint32_t kNormalsSize = 256u * 16u;
constexpr std::uint32_t kJointsOffset = 0x1040;
constexpr std::uint32_t kJointCapacity = 48;
constexpr std::uint32_t kJointsSize = kJointCapacity * 64u;
constexpr std::uint32_t kCollisionOffset = 0x1C40;
constexpr std::uint32_t kCollisionSize = 0x2000;
constexpr std::uint32_t kUsedEnd = kCollisionOffset + kCollisionSize;

static_assert(kHeaderOffset + kHeaderSize <= kNormalsOffset, "header overlaps normals");
static_assert(kNormalsOffset + kNormalsSize <= kJointsOffset, "normals overlap joints");
static_assert(kJointsOffset + kJointsSize <= kCollisionOffset, "joints overlap collision");
static_assert(kUsedEnd <= kSize, "scratchpad layout exceeds 16 KB");
static_assert(kNormalsOffset % 16u == 0 && kJointsOffset % 16u == 0
                  && kCollisionOffset % 16u == 0,
              "lv.q/sv.q need 16-byte aligned regions");

#if defined(__psp__)
inline unsigned char* Base() {
    return reinterpret_cast<unsigned char*>(static_cast<std::uintptr_t>(0x00010000u));
}
#else
alignas(64) inline unsigned char g_hostScratchpad[kSize];
inline unsigned char* Base() { return g_hostScratchpad; }
#endif

template <typename T>
inline T* At(std::uint32_t offset) {
    return reinterpret_cast<T*>(Base() + offset);
}

// Header words. Tags are non-zero constants or counters chosen by each owner.
struct Header {
    std::uint32_t magic;
    std::uint32_t normalsTag;
    std::uint32_t collisionTag;
    std::uint32_t reserved[13];
};
static_assert(sizeof(Header) == kHeaderSize, "header size");

constexpr std::uint32_t kMagic = 0x53504144u;  // "SPAD"

inline Header* GetHeader() { return At<Header>(kHeaderOffset); }

// Returns the header with a valid magic. When the magic was lost every tag
// is cleared, so every persistent region refills on its next acquire.
inline Header* ValidHeader() {
    Header* header = GetHeader();
    if (header->magic != kMagic) {
        header->normalsTag = 0;
        header->collisionTag = 0;
        header->magic = kMagic;
    }
    return header;
}

// Event counters for the host tests (refills, cache hits). Only counted on
// the host so the PSP hot loops carry no extra RAM writes.
struct Stats {
    std::uint32_t normalsRefills;
    std::uint32_t collisionHits;
    std::uint32_t collisionPromotions;
    std::uint32_t collisionEvictions;
    std::uint32_t collisionResets;
};
inline Stats g_stats = {};
#if defined(__psp__)
#define RC_PSP_SCRATCHPAD_COUNT(field) ((void)0)
#else
#define RC_PSP_SCRATCHPAD_COUNT(field) (++::PspScratchpad::g_stats.field)
#endif

// Single-owner lease for a per-draw region (no nesting: a nested user gets
// nullptr and must use its RAM fallback).
struct Lease {
    bool busy = false;
};

inline Lease g_jointsLease;

class JointsLease {
public:
    // Scratchpad storage for `count` 64-byte matrices, or nullptr.
    explicit JointsLease(std::uint32_t count) {
#if defined(RC_PSP_SCRATCHPAD)
        if (count != 0 && count <= kJointCapacity && !g_jointsLease.busy) {
            g_jointsLease.busy = true;
            data_ = Base() + kJointsOffset;
        }
#else
        (void)count;
#endif
    }
    ~JointsLease() {
        if (data_) {
            g_jointsLease.busy = false;
        }
    }
    JointsLease(const JointsLease&) = delete;
    JointsLease& operator=(const JointsLease&) = delete;
    void* data() const { return data_; }

private:
    void* data_ = nullptr;
};

} // namespace PspScratchpad
