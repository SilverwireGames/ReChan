#pragma once
#include "core.h"
#include <string>
#include <vector>

class tTexture;
class pddiRenderTarget;
class pddiBaseShader;

struct MovieSubtitleEntry {
    f64 startSeconds = 0.0;
    f64 endSeconds = 0.0;
    std::string text;
};

// MoviePlayer - plays PSX .STR files with video and audio
class MoviePlayer {
public:
    MoviePlayer();
    ~MoviePlayer();

    // Open a .str file for playback. Returns false if file not found.
    bool Open(const char* path);

    // Close and release all resources.
    void Close();

    // Advance one frame. Call once per game frame (~30fps).
    // Returns false when movie is finished or was never opened.
    bool AdvanceFrame();

    // Render the current frame fullscreen.
    void Render();

    // Check if the movie has finished playing.
    bool IsFinished() const { return finished; }

    // Check if a movie file is currently open.
    bool IsOpen() const { return fileData != nullptr; }

    // Playback frame rate (computed from sector interleave)
    f32 GetFrameRate() const { return (f32)(1.0 / frameInterval); }

private:
    // STR sector parsing
    // Sector formats: raw (2352), Mode2 without sync (2336), or headerless (2048)
    // Detected at Open() time based on file content.
    static constexpr u32 SECTOR_SIZE_RAW = 2352;    // full raw CD sector
    static constexpr u32 SECTOR_SIZE_MODE2 = 2336;  // Mode 2 without 16-byte sync+header
    static constexpr u32 SECTOR_SIZE_DATA = 2048;   // pure user data only
    static constexpr u32 VIDEO_CHUNK_HEADER = 32;   // STR frame chunk header
    u32 sectorSize = 0;       // detected sector size
    u32 subHeaderOffset = 0;  // offset to XA sub-header within sector
    u32 dataOffset = 0;       // offset to user data within sector
    u32 videoChunkData = 0;   // usable payload bytes per video sector

    struct SectorHeader {
        u8 submode;
        u8 channel;
        u8 codingInfo;
        bool isVideo;
        bool isAudio;
    };

    struct VideoChunkHeader {
        u16 chunkNum;       // sector index within frame
        u16 chunksInFrame;  // total sectors for this frame
        u32 frameNum;       // frame number (1-based)
        u32 demuxSize;      // bytes of data in demuxed frame
        u16 width;
        u16 height;
        u16 mdecCodeCount;  // MDEC codes / 2, rounded to multiple of 64
        u16 magic3800;      // always 0x3800
        u16 quantScale;     // frame quantization scale
        u16 version;        // 2 or 3
    };

    bool ParseSectorHeader(const u8* sector, SectorHeader& hdr);
    bool ParseVideoChunkHeader(const u8* sectorData, VideoChunkHeader& hdr);
    bool LoadSubtitles(const char* moviePath);
    void RenderSubtitle() const;

    // Video decoding (MDEC)
    void DecodeVideoFrame(const u8* demuxData, u32 demuxSize, u16 width, u16 height,
                          u16 quantScale, u16 version);

    // Bitstream reader for compressed data
    struct BitReader {
        const u16* data;
        u32 wordCount;
        u32 wordPos;
        u32 bitsLeft;
        u32 currentWord;

        void Init(const u8* d, u32 sizeBytes);
        s32 ReadBits(u32 n);
        u32 PeekBits(u32 n);
        void SkipBits(u32 n);
        void NextWord();
    };

    // Decode one 8x8 block: read VLC, reverse-zigzag, dequantize in one pass
    // Outputs 64 dequantized coefficients in matrix order (row-major)
    void DecodeBlock(BitReader& br, s32 block[64], u16 quantScale, u16 version,
                     s32& prevDC, bool isChroma, bool isCr);

    // IDCT matching jpsxdec's PsxMdecIDCT_int (full 64-bit precision, single >>32)
    void IDCT(const s32 block[64], s32 out[64]);
    void MacroblockToRGB(const s32 cr[64], const s32 cb[64],
                         const s32 y1[64], const s32 y2[64],
                         const s32 y3[64], const s32 y4[64],
                         u32* rgbaOut, u32 stride);

    // XA ADPCM audio
    void DecodeXAAudio(const u8* sectorData, u8 codingInfo);

    // State
    u8* fileData = nullptr;
    u32 fileSize = 0;
    u32 sectorCount = 0;
    u32 currentSector = 0;

    // Current frame assembly
    u8* demuxBuffer = nullptr;    // assembled frame data
    u32 demuxBufferSize = 0;
    u32 demuxBytesWritten = 0;
    u32 currentFrameNum = 0;
    u16 frameWidth = 0;
    u16 frameHeight = 0;
    u16 frameQuantScale = 0;
    u16 frameVersion = 0;
    u32 frameDemuxSize = 0;
    u16 frameChunksExpected = 0;
    u16 frameChunksReceived = 0;
    bool frameChunkSeen[32] = {};
    bool frameReady = false;

    // Decoded frame
    u32* rgbaBuffer = nullptr;    // RGBA pixel buffer (frameWidth * frameHeight)
    tTexture* videoTexture = nullptr;

#if MODERN_MOVIE_FILTERING
    pddiRenderTarget* denoiseFBO = nullptr;
    pddiRenderTarget* upscaleFBO = nullptr;
    pddiBaseShader*   denoiseShader = nullptr;
    pddiBaseShader*   upscaleShader = nullptr;
#endif

    // Audio state
    s16* audioPCM = nullptr;
    u32 audioPCMSize = 0;         // total allocated frames
    u32 audioPCMWritten = 0;      // frames written so far
    f64 prevSample[2][2] = {};    // XA ADPCM filter state [channel][prev1/prev2]
    u32 audioSampleRate = 37800;
    u32 audioChannels = 1;        // 1=mono, 2=stereo
    u32 audioVoice = 0;
    bool audioStarted = false;

    // Timing
    f64 playbackTime = 0.0;
    f64 frameInterval = 1.0 / 15.0; // STR typically 15fps
    u32 framesDecoded = 0;

    std::vector<MovieSubtitleEntry> subtitles;

    bool finished = false;
};
