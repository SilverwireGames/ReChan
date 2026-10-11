#pragma once
// The desktop MoviePlayer's MDEC and XA decoders (src/radmovie/movieplayer.cpp,
// MODERN_MOVIE_FILTERING off), lifted out of the class as the oracle for
// StrDecode. Keep in step with that file.
#include "core.h"
#include <algorithm>
#include <cstring>

namespace StrReference {

static const s32 IDCT_MATRIX[8][8] = {
    { 23170,  23170,  23170,  23170,  23170,  23170,  23170,  23170 },
    { 32138,  27245,  18204,   6392,  -6393, -18205, -27246, -32139 },
    { 30273,  12539, -12540, -30274, -30274, -12540,  12539,  30273 },
    { 27245,  -6393, -32139, -18205,  18204,  32138,   6392, -27246 },
    { 23170, -23171, -23171,  23170,  23170, -23171, -23171,  23170 },
    { 18204, -32139,   6392,  27245, -27246,  -6393,  32138, -18205 },
    { 12539, -30274,  30273, -12540, -12540,  30273, -30274,  12539 },
    {  6392, -18205,  27245, -32139,  32138, -27246,  18204,  -6393 }
};

static const s32 XA_K0[4] = { 0, 60, 115, 98 };
static const s32 XA_K1[4] = { 0,  0, -52, -55 };

static inline s32 SignExtend(s32 v, u32 bits) {
    s32 mask = 1 << (bits - 1);
    return (v ^ mask) - mask;
}

template <typename T> static inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct BitReader {
    const u16* data;
    u32 wordCount;
    u32 wordPos;
    u32 bitsLeft;
    u32 currentWord;

    void Init(const u8* d, u32 sizeBytes) {
        data = reinterpret_cast<const u16*>(d);
        wordCount = sizeBytes / 2;
        wordPos = 0;
        bitsLeft = 0;
        currentWord = 0;
    }
    void NextWord() {
        currentWord = (wordPos < wordCount) ? data[wordPos++] : 0;
        bitsLeft = 16;
    }
    s32 ReadBits(u32 n) {
        s32 result = 0;
        u32 remaining = n;
        while (remaining > 0) {
            if (bitsLeft == 0) NextWord();
            u32 take = std::min(remaining, bitsLeft);
            u32 shift = bitsLeft - take;
            u32 mask = ((1u << take) - 1) << shift;
            result = (result << take) | ((currentWord & mask) >> shift);
            bitsLeft -= take;
            remaining -= take;
        }
        return result;
    }
    u32 PeekBits(u32 n) {
        u32 savedWordPos = wordPos, savedBitsLeft = bitsLeft, savedWord = currentWord;
        u32 result = (u32)ReadBits(n);
        wordPos = savedWordPos;
        bitsLeft = savedBitsLeft;
        currentWord = savedWord;
        return result;
    }
    void SkipBits(u32 n) { ReadBits(n); }
};

struct VLCEntry {
    u32 code;
    u8 bits;
    u8 run;
    u8 level;
};

static const VLCEntry AC_VLC_TABLE[] = {
    { 0b11, 2, 0, 1 },
    { 0b011, 3, 1, 1 },
    { 0b0100, 4, 0, 2 }, { 0b0101, 4, 2, 1 },
    { 0b00101, 5, 0, 3 }, { 0b00110, 5, 4, 1 }, { 0b00111, 5, 3, 1 },
    { 0b000100, 6, 7, 1 }, { 0b000101, 6, 6, 1 }, { 0b000110, 6, 1, 2 }, { 0b000111, 6, 5, 1 },
    { 0b0000100, 7, 2, 2 }, { 0b0000101, 7, 9, 1 }, { 0b0000110, 7, 0, 4 }, { 0b0000111, 7, 8, 1 },
    { 0b00100000, 8, 13, 1 }, { 0b00100001, 8, 0, 6 }, { 0b00100010, 8, 12, 1 }, { 0b00100011, 8, 11, 1 },
    { 0b00100100, 8, 3, 2 }, { 0b00100101, 8, 1, 3 }, { 0b00100110, 8, 0, 5 }, { 0b00100111, 8, 10, 1 },
    { 0b0000001000, 10, 16, 1 }, { 0b0000001001, 10, 5, 2 }, { 0b0000001010, 10, 0, 7 }, { 0b0000001011, 10, 2, 3 },
    { 0b0000001100, 10, 1, 4 }, { 0b0000001101, 10, 15, 1 }, { 0b0000001110, 10, 14, 1 }, { 0b0000001111, 10, 4, 2 },
    { 0b000000010000, 12, 0, 11 }, { 0b000000010001, 12, 8, 2 }, { 0b000000010010, 12, 4, 3 }, { 0b000000010011, 12, 0, 10 },
    { 0b000000010100, 12, 2, 4 }, { 0b000000010101, 12, 7, 2 }, { 0b000000010110, 12, 21, 1 }, { 0b000000010111, 12, 20, 1 },
    { 0b000000011000, 12, 0, 9 }, { 0b000000011001, 12, 19, 1 }, { 0b000000011010, 12, 18, 1 }, { 0b000000011011, 12, 1, 5 },
    { 0b000000011100, 12, 3, 3 }, { 0b000000011101, 12, 0, 8 }, { 0b000000011110, 12, 6, 2 }, { 0b000000011111, 12, 17, 1 },
    { 0b0000000010000, 13, 10, 2 }, { 0b0000000010001, 13, 9, 2 }, { 0b0000000010010, 13, 5, 3 }, { 0b0000000010011, 13, 3, 4 },
    { 0b0000000010100, 13, 2, 5 }, { 0b0000000010101, 13, 1, 7 }, { 0b0000000010110, 13, 1, 6 }, { 0b0000000010111, 13, 0, 15 },
    { 0b0000000011000, 13, 0, 14 }, { 0b0000000011001, 13, 0, 13 }, { 0b0000000011010, 13, 0, 12 }, { 0b0000000011011, 13, 26, 1 },
    { 0b0000000011100, 13, 25, 1 }, { 0b0000000011101, 13, 24, 1 }, { 0b0000000011110, 13, 23, 1 }, { 0b0000000011111, 13, 22, 1 },
    { 0b00000000010000, 14, 0, 31 }, { 0b00000000010001, 14, 0, 30 }, { 0b00000000010010, 14, 0, 29 }, { 0b00000000010011, 14, 0, 28 },
    { 0b00000000010100, 14, 0, 27 }, { 0b00000000010101, 14, 0, 26 }, { 0b00000000010110, 14, 0, 25 }, { 0b00000000010111, 14, 0, 24 },
    { 0b00000000011000, 14, 0, 23 }, { 0b00000000011001, 14, 0, 22 }, { 0b00000000011010, 14, 0, 21 }, { 0b00000000011011, 14, 0, 20 },
    { 0b00000000011100, 14, 0, 19 }, { 0b00000000011101, 14, 0, 18 }, { 0b00000000011110, 14, 0, 17 }, { 0b00000000011111, 14, 0, 16 },
    { 0b000000000010000, 15, 0, 40 }, { 0b000000000010001, 15, 0, 39 }, { 0b000000000010010, 15, 0, 38 }, { 0b000000000010011, 15, 0, 37 },
    { 0b000000000010100, 15, 0, 36 }, { 0b000000000010101, 15, 0, 35 }, { 0b000000000010110, 15, 0, 34 }, { 0b000000000010111, 15, 0, 33 },
    { 0b000000000011000, 15, 0, 32 }, { 0b000000000011001, 15, 1, 14 }, { 0b000000000011010, 15, 1, 13 }, { 0b000000000011011, 15, 1, 12 },
    { 0b000000000011100, 15, 1, 11 }, { 0b000000000011101, 15, 1, 10 }, { 0b000000000011110, 15, 1, 9 }, { 0b000000000011111, 15, 1, 8 },
    { 0b0000000000010000, 16, 1, 18 }, { 0b0000000000010001, 16, 1, 17 }, { 0b0000000000010010, 16, 1, 16 }, { 0b0000000000010011, 16, 1, 15 },
    { 0b0000000000010100, 16, 6, 3 }, { 0b0000000000010101, 16, 16, 2 }, { 0b0000000000010110, 16, 15, 2 }, { 0b0000000000010111, 16, 14, 2 },
    { 0b0000000000011000, 16, 13, 2 }, { 0b0000000000011001, 16, 12, 2 }, { 0b0000000000011010, 16, 11, 2 }, { 0b0000000000011011, 16, 31, 1 },
    { 0b0000000000011100, 16, 30, 1 }, { 0b0000000000011101, 16, 29, 1 }, { 0b0000000000011110, 16, 28, 1 }, { 0b0000000000011111, 16, 27, 1 },
};
static constexpr u32 AC_VLC_COUNT = sizeof(AC_VLC_TABLE) / sizeof(AC_VLC_TABLE[0]);

static const u8 PSX_QUANT_FLAT[64] = {
     2, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

static const u8 REVERSE_ZIG_ZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static void DecodeBlock(BitReader& br, s32 block[64], u16 quantScale, u16 version,
                        s32& prevDC, bool isChroma) {
    std::memset(block, 0, 64 * sizeof(s32));
    s32 dc;
    if (version == 2) {
        dc = SignExtend(br.ReadBits(10), 10);
    }
    else {
        u32 peek = br.PeekBits(8);
        s32 dcDiff = 0;
        u32 sizeCode = 0;
        if (isChroma) {
            if ((peek >> 0) == 0xFE) { br.SkipBits(8); sizeCode = 8; }
            else if ((peek >> 1) >= 0x7E) { br.SkipBits(7); sizeCode = 7; }
            else if ((peek >> 2) >= 0x3E) { br.SkipBits(6); sizeCode = 6; }
            else if ((peek >> 3) >= 0x1E) { br.SkipBits(5); sizeCode = 5; }
            else if ((peek >> 4) >= 0x0E) { br.SkipBits(4); sizeCode = 4; }
            else if ((peek >> 5) >= 0x06) { br.SkipBits(3); sizeCode = 3; }
            else if ((peek >> 6) >= 0x02) { br.SkipBits(2); sizeCode = 2; }
            else if ((peek >> 6) == 0x01) { br.SkipBits(2); sizeCode = 1; }
            else { br.SkipBits(2); sizeCode = 0; }
        }
        else {
            if ((peek >> 1) >= 0x7E) { br.SkipBits(7); sizeCode = 8; }
            else if ((peek >> 2) >= 0x3E) { br.SkipBits(6); sizeCode = 7; }
            else if ((peek >> 3) >= 0x1E) { br.SkipBits(5); sizeCode = 6; }
            else if ((peek >> 4) >= 0x0E) { br.SkipBits(4); sizeCode = 5; }
            else if ((peek >> 5) >= 0x06) { br.SkipBits(3); sizeCode = 4; }
            else if ((peek >> 5) == 0x05) { br.SkipBits(3); sizeCode = 3; }
            else if ((peek >> 6) == 0x01) { br.SkipBits(2); sizeCode = 2; }
            else if ((peek >> 6) == 0x00) { br.SkipBits(2); sizeCode = 1; }
            else { br.SkipBits(3); sizeCode = 0; }
        }
        if (sizeCode != 0) {
            s32 valueBits = br.ReadBits(sizeCode);
            dcDiff = (valueBits & (1 << (sizeCode - 1))) ? valueBits : valueBits - ((1 << sizeCode) - 1);
        }
        dcDiff *= 4;
        dc = prevDC + dcDiff;
        prevDC = dc;
    }
    if (dc != 0) block[0] = dc * PSX_QUANT_FLAT[0];

    u32 vectorPos = 0;
    while (vectorPos < 63) {
        if (br.PeekBits(2) == 0x2) { br.SkipBits(2); break; }
        s32 run = 0, level = 0;
        if (br.PeekBits(6) == 0x01) {
            br.SkipBits(6);
            run = (s32)br.ReadBits(6);
            level = SignExtend(br.ReadBits(10), 10);
        }
        else {
            bool matched = false;
            for (u32 v = 0; v < AC_VLC_COUNT; v++) {
                const VLCEntry& e = AC_VLC_TABLE[v];
                u32 peeked = br.PeekBits(e.bits + 1);
                if ((peeked >> 1) == e.code) {
                    br.SkipBits(e.bits + 1);
                    level = e.level;
                    if (peeked & 1) level = -level;
                    run = e.run;
                    matched = true;
                    break;
                }
            }
            if (!matched) { br.SkipBits(1); break; }
        }
        vectorPos += run + 1;
        if (vectorPos >= 64) break;
        if (level != 0) {
            u32 matrixPos = REVERSE_ZIG_ZAG[vectorPos];
            block[matrixPos] = (level * (s32)PSX_QUANT_FLAT[matrixPos] * (s32)quantScale + 4) >> 3;
        }
    }
}

static void IDCT(const s32 block[64], s32 out[64]) {
    s64 temp[64];
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            s64 sum = 0;
            for (int i = 0; i < 8; i++) sum += (s64)block[x + i * 8] * IDCT_MATRIX[i][y];
            temp[x + y * 8] = sum;
        }
    }
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            s64 sum = 0;
            for (int i = 0; i < 8; i++) sum += IDCT_MATRIX[i][x] * temp[i + y * 8];
            out[x + y * 8] = (s32)((sum + (1LL << 31)) >> 32);
        }
    }
}

static constexpr s64 JPSXDEC_1_402 = 91893;
static constexpr s64 JPSXDEC_0_3437 = 22525;
static constexpr s64 JPSXDEC_0_7143 = 46812;
static constexpr s64 JPSXDEC_1_772 = 116224;

static inline s32 ShrRound(s64 val, int bits) {
    return (s32)((val + (1LL << (bits - 1))) >> bits);
}

static void MacroblockToRGB(const s32 cr[64], const s32 cb[64],
                            const s32 y1[64], const s32 y2[64],
                            const s32 y3[64], const s32 y4[64],
                            u32* rgbaOut, u32 stride) {
    for (int py = 0; py < 16; py++) {
        for (int px = 0; px < 16; px++) {
            s32 Y;
            int bx = px % 8, by = py % 8;
            int blockIdx = by * 8 + bx;
            if (px < 8 && py < 8) Y = y1[blockIdx];
            else if (px >= 8 && py < 8) Y = y2[blockIdx];
            else if (px < 8 && py >= 8) Y = y3[blockIdx];
            else Y = y4[blockIdx];
            int chromaIdx = (py / 2) * 8 + (px / 2);
            s32 Cr = cr[chromaIdx];
            s32 Cb = cb[chromaIdx];
            s32 Yshift = Y + 128;
            s32 r = Yshift + ShrRound(JPSXDEC_1_402 * Cr, 16);
            s32 g = Yshift + ShrRound(-JPSXDEC_0_3437 * Cb - JPSXDEC_0_7143 * Cr, 16);
            s32 b = Yshift + ShrRound(JPSXDEC_1_772 * Cb, 16);
            r = Clamp(r, (s32)0, (s32)255);
            g = Clamp(g, (s32)0, (s32)255);
            b = Clamp(b, (s32)0, (s32)255);
            rgbaOut[py * stride + px] = (255u << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
        }
    }
}

// MoviePlayer::DecodeVideoFrame for 16-aligned frames.
static void DecodeFrame(const u8* demuxData, u32 demuxSize, u16 width, u16 height, u32* rgba) {
    auto r16 = [](const u8* p) -> u16 { return p[0] | (p[1] << 8); };
    const u16 quantScale = r16(demuxData + 4);
    const u16 version = r16(demuxData + 6);
    BitReader br;
    br.Init(demuxData + 8, demuxSize - 8);
    s32 prevDC_Cr = 0, prevDC_Cb = 0, prevDC_Y = 0;
    for (u32 mbX = 0; mbX < (u32)(width + 15) / 16; mbX++) {
        for (u32 mbY = 0; mbY < (u32)(height + 15) / 16; mbY++) {
            s32 b[6][64], o[6][64];
            DecodeBlock(br, b[0], quantScale, version, prevDC_Cr, true);
            IDCT(b[0], o[0]);
            DecodeBlock(br, b[1], quantScale, version, prevDC_Cb, true);
            IDCT(b[1], o[1]);
            for (int k = 2; k < 6; ++k) {
                DecodeBlock(br, b[k], quantScale, version, prevDC_Y, false);
                IDCT(b[k], o[k]);
            }
            MacroblockToRGB(o[0], o[1], o[2], o[3], o[4], o[5], rgba + mbY * 16 * width + mbX * 16, width);
        }
    }
}

// MoviePlayer::DecodeXAAudio; returns the PCM values written.
static u32 DecodeXAAudio(const u8* sectorData, u8 codingInfo, double prevSample[2][2], s16* out) {
    bool stereo = (codingInfo & 0x01) != 0;
    u32 written = 0;
    for (u32 group = 0; group < 18; group++) {
        const u8* groupData = sectorData + group * 128;
        s16 unitSamples[8][28];
        for (u32 unit = 0; unit < 8; unit++) {
            u8 filterParam = (unit < 4) ? groupData[unit] : groupData[unit + 4];
            u32 range = filterParam & 0x0F;
            u32 filter = (filterParam >> 4) & 0x03;
            s32 k0 = XA_K0[filter];
            s32 k1 = XA_K1[filter];
            u32 ch = stereo ? (unit & 1) : 0;
            for (u32 s = 0; s < 28; s++) {
                u8 dataByte = groupData[16 + s * 4 + (unit / 2)];
                s32 nibble = (unit & 1) ? ((dataByte >> 4) & 0x0F) : (dataByte & 0x0F);
                if (nibble >= 8) nibble -= 16;
                // nibble << (12 - range) in the original; a multiply keeps UBSan quiet.
                s32 sample = nibble * (1 << (12 - range));
                sample += (k0 * (s32)prevSample[ch][0] + k1 * (s32)prevSample[ch][1]) / 64;
                sample = Clamp(sample, (s32)-32768, (s32)32767);
                prevSample[ch][1] = prevSample[ch][0];
                prevSample[ch][0] = (double)sample;
                unitSamples[unit][s] = (s16)sample;
            }
        }
        if (stereo) {
            for (u32 pair = 0; pair < 4; pair++) {
                for (u32 s = 0; s < 28; s++) {
                    out[written++] = unitSamples[pair * 2][s];
                    out[written++] = unitSamples[pair * 2 + 1][s];
                }
            }
        }
        else {
            for (u32 unit = 0; unit < 8; unit++) {
                for (u32 s = 0; s < 28; s++) out[written++] = unitSamples[unit][s];
            }
        }
    }
    return written;
}

} // namespace StrReference
