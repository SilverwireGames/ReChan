#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace PspPacketLookup {

// Validate once at load. Strictly ordered, disjoint ranges permit an exact
// predecessor lookup; unusual/overlapping assets retain first-match scanning.
template<typename Offset, typename Size>
bool IsOrdered(const Offset* bases, const Size* sizes, std::size_t count) {
    std::uint64_t previousEnd = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t base = bases[i];
        const std::uint64_t end = base + sizes[i];
        if (end > std::numeric_limits<std::uint32_t>::max()
            || (i && (base <= bases[i - 1] || base < previousEnd))) return false;
        previousEnd = end;
    }
    return true;
}

// Caller guarantees IsOrdered() and immutable packet metadata. count is the
// not-found sentinel, including gaps and empty packets. No allocations.
template<typename Offset, typename Size>
std::size_t Find(const Offset* bases, const Size* sizes, std::size_t count,
                 Offset offset) {
    std::size_t low = 0, high = count;
    while (low < high) {
        const auto mid = low + (high - low) / 2;
        if (bases[mid] <= offset) low = mid + 1;
        else high = mid;
    }
    if (low == 0) return count;
    const auto index = low - 1;
    return offset - bases[index] < sizes[index] ? index : count;
}

} // namespace PspPacketLookup
