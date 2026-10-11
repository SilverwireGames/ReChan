#include "psp/psp_strdecode.h"

#include <cstring>

namespace StrDecode {
namespace {

// MPEG-1 AC run/level codes, as in the desktop decoder; a sign bit follows
// each code.
struct AcCode {
    u16 code;
    u8 bits;
    u8 run;
    u8 level;
};

const AcCode kAcCodes[] = {
    { 0x3, 2, 0, 1 },
    { 0x3, 3, 1, 1 },
    { 0x4, 4, 0, 2 }, { 0x5, 4, 2, 1 },
    { 0x5, 5, 0, 3 }, { 0x6, 5, 4, 1 }, { 0x7, 5, 3, 1 },
    { 0x4, 6, 7, 1 }, { 0x5, 6, 6, 1 }, { 0x6, 6, 1, 2 }, { 0x7, 6, 5, 1 },
    { 0x4, 7, 2, 2 }, { 0x5, 7, 9, 1 }, { 0x6, 7, 0, 4 }, { 0x7, 7, 8, 1 },
    { 0x20, 8, 13, 1 }, { 0x21, 8, 0, 6 }, { 0x22, 8, 12, 1 }, { 0x23, 8, 11, 1 },
    { 0x24, 8, 3, 2 }, { 0x25, 8, 1, 3 }, { 0x26, 8, 0, 5 }, { 0x27, 8, 10, 1 },
    { 0x8, 10, 16, 1 }, { 0x9, 10, 5, 2 }, { 0xA, 10, 0, 7 }, { 0xB, 10, 2, 3 },
    { 0xC, 10, 1, 4 }, { 0xD, 10, 15, 1 }, { 0xE, 10, 14, 1 }, { 0xF, 10, 4, 2 },
    { 0x10, 12, 0, 11 }, { 0x11, 12, 8, 2 }, { 0x12, 12, 4, 3 }, { 0x13, 12, 0, 10 },
    { 0x14, 12, 2, 4 }, { 0x15, 12, 7, 2 }, { 0x16, 12, 21, 1 }, { 0x17, 12, 20, 1 },
    { 0x18, 12, 0, 9 }, { 0x19, 12, 19, 1 }, { 0x1A, 12, 18, 1 }, { 0x1B, 12, 1, 5 },
    { 0x1C, 12, 3, 3 }, { 0x1D, 12, 0, 8 }, { 0x1E, 12, 6, 2 }, { 0x1F, 12, 17, 1 },
    { 0x10, 13, 10, 2 }, { 0x11, 13, 9, 2 }, { 0x12, 13, 5, 3 }, { 0x13, 13, 3, 4 },
    { 0x14, 13, 2, 5 }, { 0x15, 13, 1, 7 }, { 0x16, 13, 1, 6 }, { 0x17, 13, 0, 15 },
    { 0x18, 13, 0, 14 }, { 0x19, 13, 0, 13 }, { 0x1A, 13, 0, 12 }, { 0x1B, 13, 26, 1 },
    { 0x1C, 13, 25, 1 }, { 0x1D, 13, 24, 1 }, { 0x1E, 13, 23, 1 }, { 0x1F, 13, 22, 1 },
    { 0x10, 14, 0, 31 }, { 0x11, 14, 0, 30 }, { 0x12, 14, 0, 29 }, { 0x13, 14, 0, 28 },
    { 0x14, 14, 0, 27 }, { 0x15, 14, 0, 26 }, { 0x16, 14, 0, 25 }, { 0x17, 14, 0, 24 },
    { 0x18, 14, 0, 23 }, { 0x19, 14, 0, 22 }, { 0x1A, 14, 0, 21 }, { 0x1B, 14, 0, 20 },
    { 0x1C, 14, 0, 19 }, { 0x1D, 14, 0, 18 }, { 0x1E, 14, 0, 17 }, { 0x1F, 14, 0, 16 },
    { 0x10, 15, 0, 40 }, { 0x11, 15, 0, 39 }, { 0x12, 15, 0, 38 }, { 0x13, 15, 0, 37 },
    { 0x14, 15, 0, 36 }, { 0x15, 15, 0, 35 }, { 0x16, 15, 0, 34 }, { 0x17, 15, 0, 33 },
    { 0x18, 15, 0, 32 }, { 0x19, 15, 1, 14 }, { 0x1A, 15, 1, 13 }, { 0x1B, 15, 1, 12 },
    { 0x1C, 15, 1, 11 }, { 0x1D, 15, 1, 10 }, { 0x1E, 15, 1, 9 }, { 0x1F, 15, 1, 8 },
    { 0x10, 16, 1, 18 }, { 0x11, 16, 1, 17 }, { 0x12, 16, 1, 16 }, { 0x13, 16, 1, 15 },
    { 0x14, 16, 6, 3 }, { 0x15, 16, 16, 2 }, { 0x16, 16, 15, 2 }, { 0x17, 16, 14, 2 },
    { 0x18, 16, 13, 2 }, { 0x19, 16, 12, 2 }, { 0x1A, 16, 11, 2 }, { 0x1B, 16, 31, 1 },
    { 0x1C, 16, 30, 1 }, { 0x1D, 16, 29, 1 }, { 0x1E, 16, 28, 1 }, { 0x1F, 16, 27, 1 },
};

struct AcEntry {
    u8 bits; // 0: no code starts with these bits
    u8 run;
    u8 level;
};

// Codes that do not start with six zeros are at most 8 bits long: indexed
// by the top 8 bits of the bit window. The others (10 to 16 bits) are
// indexed by the 10 bits after their six leading zeros.
struct AcTables {
    AcEntry shortCodes[256];
    AcEntry longCodes[1024];

    AcTables() : shortCodes(), longCodes() {
        for (const AcCode& c : kAcCodes) {
            const AcEntry entry = { c.bits, c.run, c.level };
            if (c.bits <= 8) {
                const u32 first = static_cast<u32>(c.code) << (8 - c.bits);
                for (u32 i = 0; i < (1u << (8 - c.bits)); ++i) shortCodes[first + i] = entry;
            }
            else {
                const u32 tailBits = c.bits - 6u;
                const u32 first = (static_cast<u32>(c.code) & ((1u << tailBits) - 1u)) << (10 - tailBits);
                for (u32 i = 0; i < (1u << (10 - tailBits)); ++i) longCodes[first + i] = entry;
            }
        }
    }
};

const AcTables kAcTables;

const u8 kQuant[64] = {
     2, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83,
};

// Coefficient order position -> row-major matrix index.
const u8 kZigZag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

// AAN scale factors for the float IDCT (libjpeg jidctflt), with its 1/8:
// kIdctScale[row * 8 + col] = s[row] * s[col] / 8, s[0] = 1,
// s[k] = cos(k*pi/16) * sqrt(2).
struct IdctScale {
    float v[64];
    IdctScale() : v() {
        const float s[8] = {
            1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
            1.0f, 0.785694958f, 0.541196100f, 0.275899379f,
        };
        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 8; ++c) v[r * 8 + c] = s[r] * s[c] * 0.125f;
        }
    }
};

const IdctScale kIdctScale;

// 8-bit channel -> 5 and 6 bits, rounded.
struct To565 {
    u8 five[256];
    u8 six[256];
    To565() : five(), six() {
        for (u32 i = 0; i < 256; ++i) {
            five[i] = static_cast<u8>((i * 31u + 127u) / 255u);
            six[i] = static_cast<u8>((i * 63u + 127u) / 255u);
        }
    }
};

const To565 kTo565;

inline s32 SignExtend(u32 v, u32 bits) {
    const s32 mask = static_cast<s32>(1u << (bits - 1));
    return (static_cast<s32>(v) ^ mask) - mask;
}

inline u8 Clamp255(s32 v) {
    return static_cast<u8>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Rounds half up, like the reference's (x + 2^31) >> 32. IDCT outputs past
// +-4096 saturate later in the colour conversion anyway.
inline s32 RoundIdct(float v) {
    if (v < -4096.0f) v = -4096.0f;
    if (v > 4095.0f) v = 4095.0f;
    return static_cast<s32>(v + 4096.5f) - 4096;
}

// MSB-first reader over little-endian 16-bit words, with a left-aligned
// 32-bit cache that always holds at least 17 bits. Past the end it reads
// zeros, like the reference.
struct Bits {
    const u8* data;
    u32 words;
    u32 pos;
    u32 cache;
    s32 count;

    void Init(const u8* d, u32 bytes) {
        data = d;
        words = bytes / 2;
        pos = 0;
        cache = 0;
        count = 0;
        Refill();
    }

    void Refill() {
        while (count <= 16) {
            u32 word = 0;
            if (pos < words) word = static_cast<u32>(data[pos * 2]) | (static_cast<u32>(data[pos * 2 + 1]) << 8);
            ++pos;
            cache |= word << (16 - count);
            count += 16;
        }
    }

    u32 Peek(u32 n) const { return cache >> (32 - n); }

    void Skip(u32 n) {
        cache <<= n;
        count -= static_cast<s32>(n);
        if (count <= 16) Refill();
    }

    u32 Read(u32 n) {
        const u32 v = Peek(n);
        Skip(n);
        return v;
    }
};

// MPEG-1 DC size codes of version 3 frames, as in the desktop decoder.
s32 ReadDcV3(Bits& br, bool chroma, s32& prevDC) {
    const u32 peek = br.Peek(8);
    u32 sizeCode = 0;
    if (chroma) {
        if (peek == 0xFE) { br.Skip(8); sizeCode = 8; }
        else if ((peek >> 1) >= 0x7E) { br.Skip(7); sizeCode = 7; }
        else if ((peek >> 2) >= 0x3E) { br.Skip(6); sizeCode = 6; }
        else if ((peek >> 3) >= 0x1E) { br.Skip(5); sizeCode = 5; }
        else if ((peek >> 4) >= 0x0E) { br.Skip(4); sizeCode = 4; }
        else if ((peek >> 5) >= 0x06) { br.Skip(3); sizeCode = 3; }
        else if ((peek >> 6) >= 0x02) { br.Skip(2); sizeCode = 2; }
        else if ((peek >> 6) == 0x01) { br.Skip(2); sizeCode = 1; }
        else { br.Skip(2); sizeCode = 0; }
    }
    else {
        if ((peek >> 1) >= 0x7E) { br.Skip(7); sizeCode = 8; }
        else if ((peek >> 2) >= 0x3E) { br.Skip(6); sizeCode = 7; }
        else if ((peek >> 3) >= 0x1E) { br.Skip(5); sizeCode = 6; }
        else if ((peek >> 4) >= 0x0E) { br.Skip(4); sizeCode = 5; }
        else if ((peek >> 5) >= 0x06) { br.Skip(3); sizeCode = 4; }
        else if ((peek >> 5) == 0x05) { br.Skip(3); sizeCode = 3; }
        else if ((peek >> 6) == 0x01) { br.Skip(2); sizeCode = 2; }
        else if ((peek >> 6) == 0x00) { br.Skip(2); sizeCode = 1; }
        else { br.Skip(3); sizeCode = 0; }
    }
    s32 diff = 0;
    if (sizeCode) {
        const s32 bits = static_cast<s32>(br.Read(sizeCode));
        diff = (bits & (1 << (sizeCode - 1))) ? bits : bits - ((1 << sizeCode) - 1);
    }
    prevDC += diff * 4;
    return prevDC;
}

// Reads one block's coefficients, dequantized into row-major `block`
// (which must start zeroed). Returns whether any AC coefficient was set.
bool DecodeBlock(Bits& br, s32 block[64], s32 qscale, u32 version, s32& prevDC, bool chroma) {
    const s32 dc = (version == 2) ? SignExtend(br.Read(10), 10) : ReadDcV3(br, chroma, prevDC);
    block[0] = dc * kQuant[0];

    bool ac = false;
    u32 pos = 0;
    while (pos < 63) {
        const u32 window = br.Peek(17);
        if ((window >> 15) == 0x2) { // end of block "10"
            br.Skip(2);
            break;
        }
        s32 run = 0;
        s32 level = 0;
        if ((window >> 11) == 0x1) { // escape "000001": 6-bit run, 10-bit level
            br.Skip(6);
            run = static_cast<s32>(br.Read(6));
            level = SignExtend(br.Read(10), 10);
        }
        else {
            const AcEntry& e = ((window >> 11) == 0)
                ? kAcTables.longCodes[(window >> 1) & 0x3FFu]
                : kAcTables.shortCodes[window >> 9];
            if (!e.bits) {
                br.Skip(1);
                break;
            }
            const u32 total = e.bits + 1u;
            const bool negative = ((window >> (17 - total)) & 1u) != 0;
            br.Skip(total);
            run = e.run;
            level = negative ? -static_cast<s32>(e.level) : static_cast<s32>(e.level);
        }
        pos += static_cast<u32>(run) + 1u;
        if (pos >= 64) break;
        if (level) {
            const u32 m = kZigZag[pos];
            block[m] = (level * static_cast<s32>(kQuant[m]) * qscale + 4) >> 3;
            ac = true;
        }
    }
    return ac;
}

// Float AAN IDCT (libjpeg jidctflt) of a dequantized row-major block into
// centred samples.
void Idct(const s32 in[64], s32 out[64]) {
    float ws[64];
    const float* q = kIdctScale.v;
    for (int c = 0; c < 8; ++c) {
        const s32* col = in + c;
        if (!(col[8] | col[16] | col[24] | col[32] | col[40] | col[48] | col[56])) {
            const float dc = static_cast<float>(col[0]) * q[c];
            for (int r = 0; r < 8; ++r) ws[r * 8 + c] = dc;
            continue;
        }
        float tmp0 = static_cast<float>(col[0]) * q[c];
        float tmp1 = static_cast<float>(col[16]) * q[16 + c];
        float tmp2 = static_cast<float>(col[32]) * q[32 + c];
        float tmp3 = static_cast<float>(col[48]) * q[48 + c];
        float tmp10 = tmp0 + tmp2;
        float tmp11 = tmp0 - tmp2;
        float tmp13 = tmp1 + tmp3;
        float tmp12 = (tmp1 - tmp3) * 1.414213562f - tmp13;
        tmp0 = tmp10 + tmp13;
        tmp3 = tmp10 - tmp13;
        tmp1 = tmp11 + tmp12;
        tmp2 = tmp11 - tmp12;

        float tmp4 = static_cast<float>(col[8]) * q[8 + c];
        float tmp5 = static_cast<float>(col[24]) * q[24 + c];
        float tmp6 = static_cast<float>(col[40]) * q[40 + c];
        float tmp7 = static_cast<float>(col[56]) * q[56 + c];
        const float z13 = tmp6 + tmp5;
        const float z10 = tmp6 - tmp5;
        const float z11 = tmp4 + tmp7;
        const float z12 = tmp4 - tmp7;
        tmp7 = z11 + z13;
        tmp11 = (z11 - z13) * 1.414213562f;
        const float z5 = (z10 + z12) * 1.847759065f;
        tmp10 = z5 - z12 * 1.082392200f;
        tmp12 = z5 - z10 * 2.613125930f;
        tmp6 = tmp12 - tmp7;
        tmp5 = tmp11 - tmp6;
        tmp4 = tmp10 - tmp5;

        ws[0 * 8 + c] = tmp0 + tmp7;
        ws[7 * 8 + c] = tmp0 - tmp7;
        ws[1 * 8 + c] = tmp1 + tmp6;
        ws[6 * 8 + c] = tmp1 - tmp6;
        ws[2 * 8 + c] = tmp2 + tmp5;
        ws[5 * 8 + c] = tmp2 - tmp5;
        ws[3 * 8 + c] = tmp3 + tmp4;
        ws[4 * 8 + c] = tmp3 - tmp4;
    }

    for (int r = 0; r < 8; ++r) {
        const float* w = ws + r * 8;
        float tmp10 = w[0] + w[4];
        float tmp11 = w[0] - w[4];
        float tmp13 = w[2] + w[6];
        float tmp12 = (w[2] - w[6]) * 1.414213562f - tmp13;
        const float tmp0 = tmp10 + tmp13;
        const float tmp3 = tmp10 - tmp13;
        const float tmp1 = tmp11 + tmp12;
        const float tmp2 = tmp11 - tmp12;

        const float z13 = w[5] + w[3];
        const float z10 = w[5] - w[3];
        const float z11 = w[1] + w[7];
        const float z12 = w[1] - w[7];
        const float tmp7 = z11 + z13;
        tmp11 = (z11 - z13) * 1.414213562f;
        const float z5 = (z10 + z12) * 1.847759065f;
        tmp10 = z5 - z12 * 1.082392200f;
        tmp12 = z5 - z10 * 2.613125930f;
        const float tmp6 = tmp12 - tmp7;
        const float tmp5 = tmp11 - tmp6;
        const float tmp4 = tmp10 - tmp5;

        s32* o = out + r * 8;
        o[0] = RoundIdct(tmp0 + tmp7);
        o[7] = RoundIdct(tmp0 - tmp7);
        o[1] = RoundIdct(tmp1 + tmp6);
        o[6] = RoundIdct(tmp1 - tmp6);
        o[2] = RoundIdct(tmp2 + tmp5);
        o[5] = RoundIdct(tmp2 - tmp5);
        o[3] = RoundIdct(tmp3 + tmp4);
        o[4] = RoundIdct(tmp3 - tmp4);
    }
}

void DecodeSamples(Bits& br, s32 out[64], s32 qscale, u32 version, s32& prevDC, bool chroma) {
    s32 block[64];
    std::memset(block, 0, sizeof(block));
    if (DecodeBlock(br, block, qscale, version, prevDC, chroma)) {
        Idct(block, out);
        return;
    }
    // DC only: every sample is DC / 8.
    const s32 v = RoundIdct(static_cast<float>(block[0]) * 0.125f);
    for (int i = 0; i < 64; ++i) out[i] = v;
}

// YCbCr -> RGB565 with the reference's fixed-point factors (jpsxdec
// PsxYCbCr_int), which fit 32 bits for any IDCT output.
void MacroblockTo565(const s32 cr[64], const s32 cb[64], const s32 (*y)[64],
                     u16* out, u32 stride, u32 w, u32 h) {
    for (u32 cy = 0; cy < 8; ++cy) {
        for (u32 cx = 0; cx < 8; ++cx) {
            const s32 vCr = cr[cy * 8 + cx];
            const s32 vCb = cb[cy * 8 + cx];
            const s32 rOff = (91893 * vCr + 32768) >> 16;
            const s32 gOff = (-22525 * vCb - 46812 * vCr + 32768) >> 16;
            const s32 bOff = (116224 * vCb + 32768) >> 16;
            for (u32 dy = 0; dy < 2; ++dy) {
                const u32 py = cy * 2 + dy;
                if (py >= h) break;
                for (u32 dx = 0; dx < 2; ++dx) {
                    const u32 px = cx * 2 + dx;
                    if (px >= w) break;
                    const s32* block = y[(py >= 8 ? 2 : 0) + (px >= 8 ? 1 : 0)];
                    const s32 luma = block[(py & 7) * 8 + (px & 7)] + 128;
                    out[py * stride + px] = static_cast<u16>(
                        kTo565.five[Clamp255(luma + rOff)]
                        | (kTo565.six[Clamp255(luma + gOff)] << 5)
                        | (kTo565.five[Clamp255(luma + bOff)] << 11));
                }
            }
        }
    }
}

// CD-XA ADPCM filter coefficients (x/64).
const s32 kXaK0[4] = { 0, 60, 115, 98 };
const s32 kXaK1[4] = { 0, 0, -52, -55 };

} // namespace

bool DecodeFrame565(const u8* frame, u32 frameBytes, u32 width, u32 height,
                    u16* out, u32 outStride) {
    if (!frame || frameBytes < 8 || !width || !height || !out) return false;
    const s32 qscale = static_cast<s32>(frame[4] | (frame[5] << 8));
    const u32 version = static_cast<u32>(frame[6] | (frame[7] << 8));
    if (version != 2 && version != 3) return false;

    Bits br;
    br.Init(frame + 8, frameBytes - 8);
    s32 prevCr = 0;
    s32 prevCb = 0;
    s32 prevY = 0;
    s32 cr[64];
    s32 cb[64];
    s32 y[4][64];
    const u32 mbW = (width + 15) / 16;
    const u32 mbH = (height + 15) / 16;
    // MDEC macroblocks run down each column first.
    for (u32 mbX = 0; mbX < mbW; ++mbX) {
        for (u32 mbY = 0; mbY < mbH; ++mbY) {
            DecodeSamples(br, cr, qscale, version, prevCr, true);
            DecodeSamples(br, cb, qscale, version, prevCb, true);
            for (u32 k = 0; k < 4; ++k) DecodeSamples(br, y[k], qscale, version, prevY, false);
            const u32 px = mbX * 16;
            const u32 py = mbY * 16;
            MacroblockTo565(cr, cb, y, out + py * outStride + px, outStride,
                            (width - px < 16) ? width - px : 16,
                            (height - py < 16) ? height - py : 16);
        }
    }
    return true;
}

u32 DecodeXaSector(const u8* data, u8 codingInfo, XaState& state, s16* out) {
    if (!data || !out || (codingInfo & 0x10)) return 0;
    const bool stereo = (codingInfo & 0x01) != 0;
    u32 written = 0;
    for (u32 group = 0; group < 18; ++group) {
        const u8* g = data + group * 128;
        s16 unitSamples[8][28];
        for (u32 unit = 0; unit < 8; ++unit) {
            // Parameters for units 0-3 are bytes 0-3, for units 4-7 bytes 8-11.
            const u8 param = g[unit < 4 ? unit : unit + 4];
            const u32 range = param & 0x0Fu;
            const u32 filter = (param >> 4) & 0x03u;
            const s32 k0 = kXaK0[filter];
            const s32 k1 = kXaK1[filter];
            s32* prev = state.prev[stereo ? (unit & 1u) : 0u];
            for (u32 s = 0; s < 28; ++s) {
                const u8 byte = g[16 + s * 4 + unit / 2];
                s32 nibble = (unit & 1u) ? ((byte >> 4) & 0x0F) : (byte & 0x0F);
                if (nibble >= 8) nibble -= 16;
                s32 sample = (range <= 12) ? nibble * (1 << (12 - range)) : (nibble >> (range - 12));
                sample += (k0 * prev[0] + k1 * prev[1]) / 64;
                if (sample < -32768) sample = -32768;
                if (sample > 32767) sample = 32767;
                prev[1] = prev[0];
                prev[0] = sample;
                unitSamples[unit][s] = static_cast<s16>(sample);
            }
        }
        if (stereo) {
            for (u32 pair = 0; pair < 4; ++pair) {
                for (u32 s = 0; s < 28; ++s) {
                    out[written++] = unitSamples[pair * 2][s];
                    out[written++] = unitSamples[pair * 2 + 1][s];
                }
            }
        }
        else {
            for (u32 unit = 0; unit < 8; ++unit) {
                for (u32 s = 0; s < 28; ++s) out[written++] = unitSamples[unit][s];
            }
        }
    }
    return stereo ? written / 2 : written;
}

} // namespace StrDecode
