#pragma once

#include "core.h"

// Decoders for PlayStation .STR movie payloads, written for the PSP's
// budget: MDEC video frames to RGB565 and CD-XA ADPCM audio to 16-bit PCM.
// They follow the desktop MoviePlayer's decoders (jpsxdec arithmetic)
// without its 64-bit and double-precision maths, which the Allegrex
// emulates in software. tests/psp/str_decode.cpp checks them against that
// reference.
namespace StrDecode {

// Decodes one demuxed MDEC frame (the payloads of its video sectors after
// their 32-byte chunk headers, concatenated) into width x height RGB565
// pixels (red in the low bits, GU_PSM_5650) at `out`, `outStride` pixels
// per row. Returns false when the data is not an MDEC v2/v3 frame.
bool DecodeFrame565(const u8* frame, u32 frameBytes, u32 width, u32 height,
                    u16* out, u32 outStride);

// CD-XA ADPCM filter history, kept across the sectors of one stream.
struct XaState {
    s32 prev[2][2] = {};
};

// PCM frames one XA sector yields: 18 groups x 8 units x 28 samples, split
// between the channels.
constexpr u32 kXaSamplesPerSector = 18 * 8 * 28;

// Decodes the 18 sound groups (2304 bytes) of one CD-XA audio sector into
// interleaved 16-bit PCM. Returns the frames written (2016 stereo or 4032
// mono), or 0 for the 8-bit format, which the original FMVs do not use.
u32 DecodeXaSector(const u8* data, u8 codingInfo, XaState& state, s16* out);

inline u32 XaChannels(u8 codingInfo) { return (codingInfo & 0x01) ? 2u : 1u; }
inline u32 XaSampleRate(u8 codingInfo) { return (codingInfo & 0x04) ? 18900u : 37800u; }

} // namespace StrDecode
