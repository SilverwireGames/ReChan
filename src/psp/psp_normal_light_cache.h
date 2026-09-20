#pragma once

#include <array>
#include <cstdint>

// Instantiate once per joint/light state. Never retain this across calls:
// normal index alone is not a valid key when a matrix or a light changes.
class PspNormalLightCache {
public:
    template <typename Evaluate>
    std::uint32_t Get(std::uint8_t normal, Evaluate&& evaluate, bool& hit) {
        const auto word = normal >> 5;
        const std::uint32_t mask = std::uint32_t{1} << (normal & 31);
        hit = (valid[word] & mask) != 0;
        if (!hit) {
            colours[normal] = evaluate(normal);
            valid[word] |= mask;
        }
        return colours[normal];
    }
private:
    // Only the 32-byte validity set is cleared; colours are read after write.
    std::array<std::uint32_t, 256> colours;
    std::array<std::uint32_t, 8> valid{};
};
