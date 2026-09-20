#include "gen/geometry.h"
#include "gen/lights.h"
#include "p3d/matrix.h"
#include "psp/psp_normal_light_cache.h"
#include "psp/psp_skin_transform_cache.h"
#include "psp/psp_packet_lookup.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

static HardwareLight lights[3];
HardwareLight::HardwareLight() = default;
HardwareLight::~HardwareLight() = default;
static u32 ambient;
u32 GetCurrentPortAmbientLightColour() { return ambient; }
const HardwareLight* GetCurrentPortHardwareLight(s32 i) { return &lights[i]; }

static void require(bool ok) { if (!ok) std::abort(); }

int main() {
    std::array<std::uint32_t, 1024> bases;
    std::array<std::uint8_t, 1024> sizes;
    for (unsigned i = 0; i < bases.size(); ++i) {
        bases[i] = i * 60u + 7u;
        sizes[i] = static_cast<std::uint8_t>((i * 17u) % 53u);
    }
    require(PspPacketLookup::IsOrdered(bases.data(), sizes.data(), bases.size()));
    for (std::uint32_t offset = 0; offset < 62000; ++offset) {
        std::size_t expected = bases.size();
        for (std::size_t i = 0; i < bases.size(); ++i) {
            if (sizes[i] && offset >= bases[i] && offset < bases[i] + sizes[i]) {
                expected = i;
                break;
            }
        }
        require(PspPacketLookup::Find(bases.data(), sizes.data(), bases.size(), offset) == expected);
    }
    require(PspPacketLookup::Find(bases.data(), sizes.data(), 0, 0u) == 0);
    const auto savedBase = bases[1];
    bases[1] = bases[0];
    require(!PspPacketLookup::IsOrdered(bases.data(), sizes.data(), bases.size()));
    bases[1] = savedBase;
    sizes[0] = 255;
    require(!PspPacketLookup::IsOrdered(bases.data(), sizes.data(), bases.size()));
    sizes[0] = 0;
    bases.back() = 0xfffffff0u;
    sizes.back() = 32;
    require(!PspPacketLookup::IsOrdered(bases.data(), sizes.data(), bases.size()));
    std::puts("PASS: exact packet lookup, 62000 offsets, gaps/empty packets, invalid range fallback guard");
    PspSkinTransformCache transforms;
    for (unsigned frame = 0; frame < 32; ++frame) {
        transforms.Begin();
        Mat4 matrix;
        matrix.m[0] = -0.75f;
        matrix.m[5] = 0.25f;
        matrix.m[12] = static_cast<float>(frame * 17);
        unsigned hits = 0;
        for (unsigned i = 0; i < 4096; ++i) {
            const auto source = static_cast<std::uint16_t>((i / 4u) % 513u);
            const float x = static_cast<float>(source) - 50.25f;
            const float y = (i & 8) ? -0.0f : 0.0f;
            const float z = (i % 13 == 0) ? 3.0f : -1.0f;
            float expected[3], actual[3];
            Mat4TransformPoint(matrix, x, y, z, expected[0], expected[1], expected[2]);
            hits += transforms.Transform(source, i % 17 == 0 ? 2u : 1u,
                matrix, x, y, z, actual[0], actual[1], actual[2]);
            require(std::memcmp(expected, actual, sizeof(expected)) == 0);
        }
        require(hits > 1000);
    }
    std::puts("PASS: exact skin transforms, collisions, changed inputs/joints, 32 frame resets");
    for (unsigned state = 0; state < 32; ++state) {
        ambient = (state * 0x193717u) & 0xffffffu;
        for (unsigned i = 0; i < 3; ++i) {
            lights[i].colour = (state * 0x371713u + i * 0x711937u) & 0xffffffu;
            lights[i].directionX = static_cast<s32>(state * 3001 + i * 5003) % 131072 - 65536;
            lights[i].directionY = -lights[i].directionX;
            lights[i].directionZ = 65536 - static_cast<s32>(state * 4096);
        }
        Mat4 matrix;
        matrix.m[0] = state & 1 ? -0.75f : 0.5f;
        matrix.m[4] = 0.25f;
        matrix.m[9] = -0.5f;
        PspNormalLightCache cache;
        unsigned evaluations = 0, hits = 0;
        for (unsigned i = 0; i < 4096; ++i) {
            const auto normal = static_cast<u8>(i * 73u);
            auto evaluate = [&](u8 n) {
                u8 r, g, b;
                ComputePsxLitColourFromNormalIndex(n, &matrix, &r, &g, &b);
                return std::uint32_t(r) | (std::uint32_t(g) << 8) | (std::uint32_t(b) << 16);
            };
            const auto expected = evaluate(normal);
            bool hit;
            const auto actual = cache.Get(normal, [&](u8 n) { ++evaluations; return evaluate(n); }, hit);
            hits += hit;
            require(actual == expected);
        }
        require(evaluations == 256 && hits == 3840);
    }
    std::puts("PASS: exact production lighting, 256 normals, 32 light/matrix states, no cross-state reuse");
}
