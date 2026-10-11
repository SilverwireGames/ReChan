// StrDecode (the PSP's .STR movie decoders) against the desktop MoviePlayer's
// decoders: random MDEC v2 bitstreams using every AC code and escapes,
// random XA sectors, and, when the user's movies are present (STR_DIR,
// default bin/fe/movies), their first frames and audio sectors.
#include "psp/psp_strdecode.h"
#include "str_reference.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static int s_failures = 0;

static void Fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++s_failures;
}

struct BitWriter {
    std::vector<u8> bytes;
    u32 word = 0;
    u32 used = 0;

    void Put(u32 value, u32 bits) {
        for (u32 i = bits; i-- > 0;) {
            word = (word << 1) | ((value >> i) & 1u);
            if (++used == 16) {
                bytes.push_back(static_cast<u8>(word & 0xFF));
                bytes.push_back(static_cast<u8>(word >> 8));
                word = 0;
                used = 0;
            }
        }
    }
    void Flush() {
        while (used) Put(0, 1);
        for (int i = 0; i < 8; ++i) Put(0, 16); // padding the decoders may peek at
    }
};

// One random v2 frame: every block has a DC and a run of AC events drawn
// from the whole code table and the escape code, ended by EOB.
static std::vector<u8> RandomFrame(std::mt19937& rng, u32 width, u32 height, u16 qscale) {
    BitWriter w;
    const u32 blocks = ((width + 15) / 16) * ((height + 15) / 16) * 6;
    for (u32 b = 0; b < blocks; ++b) {
        w.Put(rng() & 0x3FFu, 10);
        u32 pos = 0;
        const u32 events = rng() % 12;
        for (u32 e = 0; e < events; ++e) {
            if (rng() % 8 == 0) {
                const u32 run = rng() % 8;
                if (pos + run + 1 > 62) break;
                const s32 level = static_cast<s32>(rng() % 129) - 64;
                w.Put(0x1, 6);
                w.Put(run, 6);
                w.Put(static_cast<u32>(level) & 0x3FFu, 10);
                pos += run + 1;
            }
            else {
                const StrReference::VLCEntry& c = StrReference::AC_VLC_TABLE[rng() % StrReference::AC_VLC_COUNT];
                if (pos + c.run + 1 > 62) break;
                w.Put(c.code, c.bits);
                w.Put(rng() & 1u, 1);
                pos += c.run + 1;
            }
        }
        w.Put(0x2, 2);
    }
    w.Flush();
    std::vector<u8> frame(8);
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0x00;
    frame[3] = 0x38;
    frame[4] = static_cast<u8>(qscale & 0xFF);
    frame[5] = static_cast<u8>(qscale >> 8);
    frame[6] = 2;
    frame[7] = 0;
    frame.insert(frame.end(), w.bytes.begin(), w.bytes.end());
    return frame;
}

struct FrameStats {
    u32 pixels = 0;
    u32 mismatched = 0;
    u32 maxDiff = 0;
};

static void CompareFrame(const std::vector<u8>& frame, u32 width, u32 height, FrameStats& stats,
                         double* refSeconds, double* fastSeconds) {
    std::vector<u32> rgba(width * height, 0);
    std::vector<u16> fast(width * height, 0);
    const auto t0 = std::chrono::steady_clock::now();
    StrReference::DecodeFrame(frame.data(), static_cast<u32>(frame.size()), static_cast<u16>(width),
                              static_cast<u16>(height), rgba.data());
    const auto t1 = std::chrono::steady_clock::now();
    if (!StrDecode::DecodeFrame565(frame.data(), static_cast<u32>(frame.size()), width, height, fast.data(), width)) {
        Fail("DecodeFrame565 rejected a v2 frame");
        return;
    }
    const auto t2 = std::chrono::steady_clock::now();
    if (refSeconds) *refSeconds += std::chrono::duration<double>(t1 - t0).count();
    if (fastSeconds) *fastSeconds += std::chrono::duration<double>(t2 - t1).count();

    for (u32 i = 0; i < width * height; ++i) {
        const u32 c = rgba[i];
        const u32 r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
        const u32 er = (r * 31 + 127) / 255, eg = (g * 63 + 127) / 255, eb = (b * 31 + 127) / 255;
        const u32 p = fast[i];
        const u32 fr = p & 0x1F, fg = (p >> 5) & 0x3F, fb = p >> 11;
        const u32 d = std::max({ fr > er ? fr - er : er - fr, fg > eg ? fg - eg : eg - fg,
                                 fb > eb ? fb - eb : eb - fb });
        ++stats.pixels;
        if (d) ++stats.mismatched;
        if (d > stats.maxDiff) stats.maxDiff = d;
    }
}

static void CheckStats(const FrameStats& stats, const char* label) {
    const double share = stats.pixels ? (double)stats.mismatched / stats.pixels : 0.0;
    std::printf("%s: %u pixels, %.4f%% differ, max diff %u (565 units)\n", label, stats.pixels,
                share * 100.0, stats.maxDiff);
    if (stats.maxDiff > 1) Fail(label);
    if (share > 0.01) Fail(label);
}

static void CompareXa(const u8* sector, u8 coding, StrDecode::XaState& fastState, double refState[2][2],
                      const char* label) {
    s16 ref[StrDecode::kXaSamplesPerSector];
    s16 fast[StrDecode::kXaSamplesPerSector];
    const u32 refValues = StrReference::DecodeXAAudio(sector, coding, refState, ref);
    const u32 frames = StrDecode::DecodeXaSector(sector, coding, fastState, fast);
    const u32 values = frames * StrDecode::XaChannels(coding);
    if (values != refValues || std::memcmp(ref, fast, values * sizeof(s16)) != 0) Fail(label);
}

static u32 ReadU32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24); }
static u16 ReadU16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }

// Demuxes the first frames of a Mode 2 (2336-byte sector) .STR file and
// compares both video and audio decoders on them.
static bool CheckMovie(const std::string& path, u32 maxFrames, FrameStats& stats, double* refSeconds,
                       double* fastSeconds) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const u32 sectorSize = 2336;
    std::vector<u8> sector(sectorSize);
    std::vector<u8> frame;
    u32 frameNum = 0xFFFFFFFFu, chunks = 0, received = 0, width = 0, height = 0, frames = 0, demux = 0;
    StrDecode::XaState xa;
    double refXa[2][2] = {};
    while (frames < maxFrames && std::fread(sector.data(), 1, sectorSize, f) == sectorSize) {
        const u8 submode = sector[2];
        const u8 coding = sector[3];
        const u8* data = sector.data() + 8;
        if (submode & 0x04) {
            CompareXa(data, coding, xa, refXa, "real XA sector matches the reference");
            continue;
        }
        if ((ReadU32(data) & 0xFFFFFF) != 0x010160) continue;
        const u16 chunk = ReadU16(data + 4);
        const u16 count = ReadU16(data + 6);
        const u32 num = ReadU32(data + 8);
        if (num != frameNum) {
            if (received && received >= chunks) {
                frame.resize(demux);
                CompareFrame(frame, width, height, stats, refSeconds, fastSeconds);
                ++frames;
            }
            frameNum = num;
            chunks = count;
            received = 0;
            demux = ReadU32(data + 12);
            width = ReadU16(data + 16);
            height = ReadU16(data + 18);
            frame.clear();
        }
        if (chunk < count) {
            const u8 form2 = submode & 0x20;
            const u32 payload = (form2 ? 2328u : 2048u) - 32u;
            frame.insert(frame.end(), data + 32, data + 32 + payload);
            ++received;
        }
    }
    std::fclose(f);
    return frames > 0;
}

int main() {
    std::mt19937 rng(0x5EED1234u);

    FrameStats synthetic;
    for (int i = 0; i < 24; ++i) {
        const std::vector<u8> frame = RandomFrame(rng, 320, 240, static_cast<u16>(1 + rng() % 16));
        CompareFrame(frame, 320, 240, synthetic, nullptr, nullptr);
    }
    CheckStats(synthetic, "synthetic MDEC v2 frames");

    // Extreme values must stay in bounds (the result is not compared).
    {
        BitWriter w;
        for (u32 b = 0; b < 300 * 6; ++b) {
            w.Put(0x1FF, 10);
            for (int e = 0; e < 3; ++e) {
                w.Put(0x1, 6);
                w.Put(0, 6);
                w.Put(0x1FF, 10);
            }
            w.Put(0x2, 2);
        }
        w.Flush();
        std::vector<u8> frame = { 0, 0, 0, 0x38, 63, 0, 2, 0 };
        frame.insert(frame.end(), w.bytes.begin(), w.bytes.end());
        std::vector<u16> out(320 * 240);
        StrDecode::DecodeFrame565(frame.data(), static_cast<u32>(frame.size()), 320, 240, out.data(), 320);
        // Truncated data reads zeros, never past the buffer.
        StrDecode::DecodeFrame565(frame.data(), 64, 320, 240, out.data(), 320);
    }

    for (u8 coding : { static_cast<u8>(0x05), static_cast<u8>(0x01), static_cast<u8>(0x00) }) {
        StrDecode::XaState fast;
        double ref[2][2] = {};
        std::vector<u8> sector(2304);
        for (int s = 0; s < 40; ++s) {
            for (u32 i = 0; i < sector.size(); ++i) sector[i] = static_cast<u8>(rng());
            for (u32 g = 0; g < 18; ++g) {
                for (u32 p = 0; p < 16; ++p) {
                    u8& param = sector[g * 128 + p];
                    param = static_cast<u8>(((param >> 4) & 0x3) << 4 | (param & 0x0F) % 13);
                }
            }
            CompareXa(sector.data(), coding, fast, ref, "random XA sector matches the reference");
        }
    }

    const char* dir = std::getenv("STR_DIR");
    const std::string movieDir = dir ? dir : "bin/fe/movies";
    FrameStats real;
    double refSeconds = 0.0, fastSeconds = 0.0;
    u32 movies = 0;
    for (const char* name : { "radi.str", "mdwy320m.str", "dolby.str", "prolog.str", "credits.str" }) {
        if (CheckMovie(movieDir + "/" + name, 40, real, &refSeconds, &fastSeconds)) ++movies;
    }
    if (movies) {
        CheckStats(real, "movie frames");
        std::printf("%u movies: reference %.1f ms/frame, StrDecode %.1f ms/frame (host)\n", movies,
                    refSeconds * 1000.0 / (real.pixels / (320.0 * 240.0)),
                    fastSeconds * 1000.0 / (real.pixels / (320.0 * 240.0)));
    }
    else {
        std::printf("no movies under %s: real-frame check skipped\n", movieDir.c_str());
    }

    if (s_failures) return 1;
    std::printf("PASS: STR decoders match the desktop reference\n");
    return 0;
}
