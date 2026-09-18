#pragma once
#include "core.h"
#include <vector>

namespace SpuAdpcm {
    // PSX SPU ADPCM filter coefficients (fixed-point pairs)
    // filter 0: (0, 0)  filter 1: (60, 0)  filter 2: (115, -52)
    // filter 3: (98, -55)  filter 4: (122, -60)
    static constexpr s32 FILTER_POS[5] = { 0, 60, 115, 98, 122 };
    static constexpr s32 FILTER_NEG[5] = { 0, 0, -52, -55, -60 };

    // Decode a single 16-byte ADPCM block into 28 s16 samples
    // Returns true if this block has the END flag set
    inline bool DecodeBlock(const u8* block, s16* out, s32& prev0, s32& prev1) {
        u8 shiftFilter = block[0];
        u8 flags = block[1];

        u32 shift = shiftFilter & 0x0F;
        u32 filter = (shiftFilter >> 4) & 0x0F;
        if (filter > 4) filter = 4;
        if (shift > 12) shift = 12;

        s32 f0 = FILTER_POS[filter];
        s32 f1 = FILTER_NEG[filter];

        for (u32 i = 0; i < 28; i++) {
            // Each byte has two 4-bit nibbles (low nibble first)
            u32 byteIdx = 2 + (i / 2);
            s32 nibble;
            if ((i & 1) == 0) {
                nibble = (s32)(block[byteIdx] & 0x0F);
            }
            else {
                nibble = (s32)(block[byteIdx] >> 4);
            }

            // Sign-extend from 4 bits
            if (nibble >= 8) nibble -= 16;

            // Scale by shift
            s32 sample = (nibble << 12) >> shift;

            // Apply filter
            sample += (prev0 * f0 + prev1 * f1 + 32) >> 6;

            // Clamp to s16
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;

            out[i] = (s16)sample;
            prev1 = prev0;
            prev0 = sample;
        }

        return (flags & 0x01) != 0; // END flag
    }

    // Decode an entire SPU ADPCM stream into PCM s16
    // adpcmData: pointer to ADPCM blocks (must be 16-byte aligned blocks)
    // adpcmSize: size in bytes (must be multiple of 16)
    // stopAtEnd: if true, stop at first END-flagged block; if false, decode all blocks
    // Returns decoded s16 PCM samples
    inline std::vector<s16> Decode(const u8* adpcmData, u32 adpcmSize, bool stopAtEnd = false) {
        u32 numBlocks = adpcmSize / 16;
        std::vector<s16> pcm;

        // WAX descriptors point into a shared ADPCM pool. Reserving the
        // entire tail for every descriptor can multiply memory use by the
        // number of samples (RS0000 alone reserves ~17 MB for ~0.5 MB of
        // decoded PCM). When an END marker is requested, scan the compact
        // block headers first and reserve only the selected sample.
        u32 reserveBlocks = numBlocks;
        if (stopAtEnd) {
            reserveBlocks = 0;
            while (reserveBlocks < numBlocks) {
                const u8 flags = adpcmData[reserveBlocks * 16 + 1];
                ++reserveBlocks;
                if (flags & 0x01) break;
            }
        }
        pcm.reserve(static_cast<size_t>(reserveBlocks) * 28u);

        s32 prev0 = 0, prev1 = 0;
        for (u32 b = 0; b < numBlocks; b++) {
            s16 blockOut[28];
            bool end = DecodeBlock(adpcmData + b * 16, blockOut, prev0, prev1);
            for (u32 i = 0; i < 28; i++) {
                pcm.push_back(blockOut[i]);
            }
            if (end && stopAtEnd) break;
        }

        return pcm;
    }

    // Decode an SPU ADPCM blob into individual samples, splitting at END flags
    // Each sample's filter state is independent (prev0/prev1 reset between samples)
    // Returns vector of individual PCM sample buffers
    inline std::vector<std::vector<s16>> DecodeSamples(const u8* adpcmData, u32 adpcmSize) {
        std::vector<std::vector<s16>> samples;
        u32 numBlocks = adpcmSize / 16;

        s32 prev0 = 0, prev1 = 0;
        std::vector<s16> current;

        for (u32 b = 0; b < numBlocks; b++) {
            s16 blockOut[28];
            bool end = DecodeBlock(adpcmData + b * 16, blockOut, prev0, prev1);
            for (u32 i = 0; i < 28; i++) {
                current.push_back(blockOut[i]);
            }
            if (end) {
                if (!current.empty()) {
                    samples.push_back(std::move(current));
                    current.clear();
                }
                prev0 = 0;
                prev1 = 0;
            }
        }
        // Trailing data after last END flag
        if (!current.empty()) {
            samples.push_back(std::move(current));
        }

        return samples;
    }

}
