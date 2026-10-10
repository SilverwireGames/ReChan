#pragma once
#include "core.h"
#include "snd/adpcm.h"
#include <vector>

// rsdformat.h - PSX rsd sound format loaders (WAX, FAG, CLP)
// Parses the TLV block format used by Radical Sound (rsd) library
// WAX = sound effects bank, FAG = streaming music, CLP = ambient clips

namespace RsdFormat {
    // TLV block tags (from rsdLoadData switch table, RSDBACH.CPP)
    static constexpr u32 TAG_ADPCM_BLOCK = 16;  // 0x10: SPU ADPCM data block
    static constexpr u32 TAG_SAMPLE_TABLE = 17; // 0x11: per-sample descriptor table
    static constexpr u32 TAG_HEADER = 32;       // 0x20: bank header

    // Per-sample descriptor from tag=17 blocks
    struct SampleDesc {
        u32 spuAddr;     // SPU RAM address where sample lives
        u32 params;      // pitch/rate/ADSR packed field
        u32 flags;       // loop flags (0 = no loop, 0xC0000 = loop)
        u32 spuAddr2;    // duplicate of spuAddr (or loop address)
    };

    // Decoded WAX bank
    struct WaxBank {
        std::vector<std::vector<s16>> pcmSamples;   // individual decoded PCM samples
        std::vector<SampleDesc> sampleDescs;        // per-sample descriptors from tag=17
        u32 adpcmOffset = 0;
        u32 adpcmSize = 0;
        u32 numBankSamples = 0;                     // from tag=32 header
    };

    // Load a WAX file - returns decoded bank
    inline WaxBank LoadWax(const u8* fileData, u32 fileSize) {
        WaxBank bank;
        const u8* adpcmPtr = nullptr;
        u32 adpcmLen = 0;

        // Walk TLV blocks
        u32 pos = 0;
        while (pos + 8 <= fileSize) {
            u32 tag = *(const u32*)(fileData + pos);
            u32 dataSize = *(const u32*)(fileData + pos + 4);
            const u8* data = fileData + pos + 8;

            if (pos + 8 + dataSize > fileSize) break;

            if (tag == TAG_HEADER && dataSize >= 4) {
                // Header block: data[0]=numSamples, data[1]=???, data[2]=???
                bank.numBankSamples = *(const u32*)(data);
            }
            else if (tag == TAG_ADPCM_BLOCK && dataSize > 4) {
                // PSX: data[0..3] = SPU destination address, data[4..] = ADPCM data
                // rsdLoadData passes (i+3, i[1]-4) to rsdLoad, skipping the 4-byte SPU addr
                bank.adpcmOffset = *(const u32*)(data);
                adpcmPtr = data + 4;
                adpcmLen = dataSize - 4;
                bank.adpcmSize = adpcmLen;
            }
            else if (tag == TAG_SAMPLE_TABLE && dataSize >= 16) {
                // Sample table: array of {spuAddr, params, flags, spuAddr2}
                u32 numSamples = dataSize / 16;
                bank.sampleDescs.resize(numSamples);
                memcpy(bank.sampleDescs.data(), data, numSamples * 16);
            }

            pos += 8 + dataSize;
        }

        // Decode ADPCM into individual samples using descriptor SPU addresses
        // Descriptors map sample index -> SPU address within the ADPCM block.
        // Samples are NOT stored sequentially by index, so END-flag splitting won't work.
        if (adpcmPtr && adpcmLen > 0 && !bank.sampleDescs.empty()) {
            u32 spuBase = bank.adpcmOffset;
            u32 numDescs = (u32)bank.sampleDescs.size();
            bank.pcmSamples.resize(numDescs);

            for (u32 i = 0; i < numDescs; i++) {
                u32 spuAddr = bank.sampleDescs[i].spuAddr;
                if (spuAddr < spuBase) continue;
                u32 byteOff = spuAddr - spuBase;
                if (byteOff >= adpcmLen) continue;
                bank.pcmSamples[i] = SpuAdpcm::Decode(adpcmPtr + byteOff, adpcmLen - byteOff, true);
            }
        }
        else if (adpcmPtr && adpcmLen > 0) {
            // Fallback: no descriptors, split at END flags
            bank.pcmSamples = SpuAdpcm::DecodeSamples(adpcmPtr, adpcmLen);
        }

        return bank;
    }

    // PSP: header-only WAX index (no PCM decode). Gives each sample's SPU address,
    // flags and pitch, plus the file offset of the shared ADPCM pool, so one sample
    // at a time can be read and decoded on demand instead of holding every bank in
    // the 128-slot mixer.
    struct WaxIndex {
        std::vector<SampleDesc> sampleDescs;
        u32 spuBase = 0;
        u32 adpcmFileOffset = 0;
        u32 adpcmLength = 0;
    };

    inline WaxIndex ParseWaxIndex(const u8* fileData, u32 fileSize) {
        WaxIndex index;
        u32 pos = 0;
        while (pos + 8 <= fileSize) {
            u32 tag = *(const u32*)(fileData + pos);
            u32 dataSize = *(const u32*)(fileData + pos + 4);
            const u8* data = fileData + pos + 8;

            if (pos + 8 + dataSize > fileSize) break;

            if (tag == TAG_ADPCM_BLOCK && dataSize > 4) {
                index.spuBase = *(const u32*)(data);
                index.adpcmFileOffset = pos + 8 + 4;
                index.adpcmLength = dataSize - 4;
            }
            else if (tag == TAG_SAMPLE_TABLE && dataSize >= 16) {
                u32 numSamples = dataSize / 16;
                index.sampleDescs.resize(numSamples);
                memcpy(index.sampleDescs.data(), data, numSamples * 16);
            }

            pos += 8 + dataSize;
        }
        return index;
    }

    // Decoded CLP ambient bank
    struct ClpBank {
        struct Clip {
            std::vector<s16> pcmData;
        };
        std::vector<Clip> clips;
    };

    // Load a CLP file - simple offset table + ADPCM clips
    inline ClpBank LoadClp(const u8* fileData, u32 fileSize) {
        ClpBank bank;
        if (fileSize < 4) return bank;

        u32 numClips = *(const u32*)(fileData);
        if (numClips == 0 || numClips > 256) return bank;

        // Offset table: u32[numClips] start offsets, then u32[numClips] end offsets
        if (fileSize < 4 + numClips * 8) return bank;

        const u32* startOffsets = (const u32*)(fileData + 4);
        const u32* endOffsets = (const u32*)(fileData + 4 + numClips * 4);

        bank.clips.resize(numClips);
        for (u32 i = 0; i < numClips; i++) {
            u32 start = startOffsets[i];
            u32 end = endOffsets[i];
            if (start >= fileSize || end > fileSize || end <= start) continue;

            u32 adpcmSize = end - start;
            bank.clips[i].pcmData = SpuAdpcm::Decode(fileData + start, adpcmSize);
        }

        return bank;
    }

    // Decoded FAG music track
    struct FagTrack {
        std::vector<s16> pcmData; // interleaved stereo s16 PCM (L,R,L,R,...)
        u32 numFrames = 0;        // number of stereo frames (pcmData.size() / channels)
        u32 channels = 2;         // always stereo for FAG
        u32 sampleRate = 22050;
    };

    // Load a FAG file - streaming music with whole-chunk stereo interleave
    // FAG header: { u32 numSongs, u32 songStarts[numSongs], u32 songEnds[numSongs] }
    // PSX rsdStream dualVoice format: each 2048-block (32KB) chunk alternates channels.
    // Even chunks (0,2,4...) = Left channel, Odd chunks (1,3,5...) = Right channel.
    // Filter state resets at each chunk boundary (block 0 of each chunk has shift=0, filter=0).
    inline FagTrack LoadFag(const u8* fileData, u32 fileSize, u32 songIndex = 0) {
        FagTrack track;
        if (fileSize < 4) return track;

        // Parse FAG header (matches rsdMusicPlayer::Open)
        u32 numSongs = *(const u32*)(fileData);
        if (numSongs == 0 || numSongs > 64) return track;

        u32 headerSize = 4 + numSongs * 8;
        if (fileSize < headerSize) return track;

        const u32* songStarts = (const u32*)(fileData + 4);
        const u32* songEnds = (const u32*)(fileData + 4 + numSongs * 4);

        if (songIndex >= numSongs) songIndex = 0;
        u32 dataOffset = songStarts[songIndex];
        u32 dataEnd = songEnds[songIndex];
        if (dataOffset >= fileSize || dataEnd <= dataOffset) return track;
        if (dataEnd > fileSize) dataEnd = fileSize;

        u32 dataSize = dataEnd - dataOffset;
        const u8* adpcmData = fileData + dataOffset;

        // Whole-chunk stereo de-interleave
        static constexpr u32 CHUNK_BLOCKS = 2048;
        static constexpr u32 CHUNK_BYTES = CHUNK_BLOCKS * 16; // 32KB
        static constexpr u32 SAMPLES_PER_CHUNK = CHUNK_BLOCKS * 28;

        u32 totalBlocks = dataSize / 16;
        u32 numChunks = totalBlocks / CHUNK_BLOCKS;
        u32 numPairs = numChunks / 2;

#if defined(RC_PLATFORM_PSP)
        // The PSP cannot afford a second full decoded copy of a music track.
        // Decode one L/R pair at a time, downmix to mono and keep every other
        // source frame. The native output is still mixed at 44.1 kHz, while
        // music uses its original 11.025 kHz equivalent with interpolation.
        const u32 sourceFrames = numPairs * SAMPLES_PER_CHUNK;
        const u32 framesPerPair = SAMPLES_PER_CHUNK;
        track.numFrames = sourceFrames / 2;
        track.channels = 1;
        track.sampleRate = 11025;
        track.pcmData.resize(track.numFrames);

        std::vector<s16> left(SAMPLES_PER_CHUNK);
        std::vector<s16> right(SAMPLES_PER_CHUNK);
        for (u32 p = 0; p < numPairs; p++) {
            const u32 chunkIndices[2] = {p * 2, p * 2 + 1};
            std::vector<s16>* channels[2] = {&left, &right};
            for (u32 channel = 0; channel < 2; channel++) {
                const u8* chunkBase = adpcmData + chunkIndices[channel] * CHUNK_BYTES;
                s32 prev0 = 0, prev1 = 0;
                for (u32 b = 0; b < CHUNK_BLOCKS; b++) {
                    s16 blockOut[28];
                    SpuAdpcm::DecodeBlock(chunkBase + b * 16, blockOut, prev0, prev1);
                    memcpy(&(*channels[channel])[b * 28], blockOut, 28 * sizeof(s16));
                }
            }

            s16* dst = &track.pcmData[p * (framesPerPair / 2)];
            for (u32 i = 0; i < framesPerPair; i += 2) {
                const s32 mixed = static_cast<s32>(left[i]) + static_cast<s32>(right[i]);
                dst[i / 2] = static_cast<s16>(mixed / 2);
            }
        }
#else
        // Interleave even chunks (L) with odd chunks (R) into stereo PCM.
        u32 framesPerPair = SAMPLES_PER_CHUNK;
        track.numFrames = numPairs * framesPerPair;
        track.channels = 2;
        track.pcmData.resize(track.numFrames * 2);

        // Decode each chunk independently (filter state resets per chunk).
        std::vector<std::vector<s16>> chunkPcm(numChunks);
        for (u32 c = 0; c < numChunks; c++) {
            const u8* chunkBase = adpcmData + c * CHUNK_BYTES;
            s32 prev0 = 0, prev1 = 0;
            chunkPcm[c].resize(SAMPLES_PER_CHUNK);
            for (u32 b = 0; b < CHUNK_BLOCKS; b++) {
                s16 blockOut[28];
                SpuAdpcm::DecodeBlock(chunkBase + b * 16, blockOut, prev0, prev1);
                memcpy(&chunkPcm[c][b * 28], blockOut, 28 * sizeof(s16));
            }
        }

        for (u32 p = 0; p < numPairs; p++) {
            const s16* left = chunkPcm[p * 2].data();
            const s16* right = chunkPcm[p * 2 + 1].data();
            s16* dst = &track.pcmData[p * framesPerPair * 2];
            for (u32 i = 0; i < framesPerPair; i++) {
                dst[i * 2] = left[i];
                dst[i * 2 + 1] = right[i];
            }
        }
#endif

        return track;
    }
}
