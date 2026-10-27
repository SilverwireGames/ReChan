#include "gen/common.h"
#include "radmovie/movieplayer.h"
#include "pc/audio.h"
#include "pc/textmgr.h"
#include "pc/tim.h"
#ifdef MOD_LOADER
#include "extra/modloader.h"
#endif
#include "p3d/texture.h"
#include "p3d/context.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include "pddi/pddishad.h"
#include "pddi/pdditex.h"
#include "xclib/xcfile.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <filesystem>

// Reference: "The PlayStation 1 Video (STR) Format" v1.20 by Michael Sabin (MIT license)

// Fixed-point IDCT cosine matrix (values * 65536)
// Matches jpsxdec PSX_DEFAULT_COSINE_MATRIX
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

// XA ADPCM filter coefficients (fixed-point, multiply by 1/64)
static const s32 XA_K0[4] = { 0, 60, 115, 98 };
static const s32 XA_K1[4] = { 0,  0, -52, -55 };

// Sign-extend n-bit value
static inline s32 SignExtend(s32 v, u32 bits) {
    s32 mask = 1 << (bits - 1);
    return (v ^ mask) - mask;
}

// Movie playback sync state is kept here so movieplayer.h does not need
// extra members just to make video catch up after render stalls/window resize.
struct MoviePlaybackSyncState {
    bool clockStarted = false;
    f64 startTimeSeconds = 0.0;
};

static std::unordered_map<const MoviePlayer*, MoviePlaybackSyncState> s_moviePlaybackSync;

static f64 MovieNowSeconds() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point s_start = Clock::now();

    const std::chrono::duration<f64> elapsed = Clock::now() - s_start;
    return elapsed.count();
}

static void MovieSyncReset(const MoviePlayer* movie) {
    s_moviePlaybackSync.erase(movie);
}

static MoviePlaybackSyncState& MovieSyncState(const MoviePlayer* movie) {
    return s_moviePlaybackSync[movie];
}

static bool MovieSyncIsStarted(const MoviePlayer* movie) {
    const auto it = s_moviePlaybackSync.find(movie);
    return it != s_moviePlaybackSync.end() && it->second.clockStarted;
}

static void MovieSyncStartIfNeeded(const MoviePlayer* movie) {
    MoviePlaybackSyncState& state = MovieSyncState(movie);

    if (!state.clockStarted) {
        state.clockStarted = true;
        state.startTimeSeconds = MovieNowSeconds();
    }
}

static f64 MovieSyncElapsedSeconds(const MoviePlayer* movie) {
    const auto it = s_moviePlaybackSync.find(movie);

    if (it == s_moviePlaybackSync.end() || !it->second.clockStarted) {
        return 0.0;
    }

    return MovieNowSeconds() - it->second.startTimeSeconds;
}

static std::string TrimMovieSubtitleText(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

static bool ParseMovieSubtitleTime(const std::string& value, f64& outSeconds) {
    unsigned int hours = 0, minutes = 0, seconds = 0, milliseconds = 0;
    if (std::sscanf(value.c_str(), "%u:%u:%u,%u", &hours, &minutes, &seconds, &milliseconds) != 4) {
        return false;
    }
    outSeconds = (f64)hours * 3600.0 + (f64)minutes * 60.0
        + (f64)seconds + (f64)milliseconds / 1000.0;
    return true;
}

static bool ParseMovieSubtitleTiming(const std::string& line, f64& outStart, f64& outEnd) {
    const size_t separator = line.find("-->");
    if (separator == std::string::npos) return false;
    const std::string start = TrimMovieSubtitleText(line.substr(0, separator));
    const std::string end = TrimMovieSubtitleText(line.substr(separator + 3));
    return ParseMovieSubtitleTime(start, outStart) && ParseMovieSubtitleTime(end, outEnd)
        && outEnd >= outStart;
}


// MoviePlayer implementation

MoviePlayer::MoviePlayer() {}

MoviePlayer::~MoviePlayer() {
    Close();
}

bool MoviePlayer::Open(const char* path) {
    Close();
    MovieSyncReset(this);

    u8* rawData = nullptr;
    if (!xcReadFileLow(path, &rawData, &fileSize)) {
        LOG("[MoviePlayer] Cannot open: %s", path);
        return false;
    }

    if (fileSize < 256) {
        LOG("[MoviePlayer] File too small: %s (%u bytes)", path, fileSize);
        delete[] rawData;
        return false;
    }

    fileData = rawData;
    LoadSubtitles(path);

    // Auto-detect sector format from file content
    // Raw 2352: starts with CD sync 00 FF FF FF FF FF FF FF FF FF FF 00
    // Mode2 2336: starts with XA sub-header (8 bytes), STR magic at offset 8
    // Data 2048: STR magic directly at offset 0
    auto r32 = [](const u8* p) -> u32 { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); };

    if (fileSize >= SECTOR_SIZE_RAW && fileData[0] == 0x00 && fileData[1] == 0xFF && fileData[11] == 0x00) {
        // Raw 2352-byte sectors with full CD sync+header
        sectorSize = SECTOR_SIZE_RAW;
        subHeaderOffset = 16;  // after 12 sync + 4 header
        dataOffset = 24;       // after sync + header + sub-header (8 bytes)
        LOG("[MoviePlayer] Detected raw 2352-byte sector format");
    }
    else if (fileSize >= SECTOR_SIZE_MODE2 && (fileSize % SECTOR_SIZE_MODE2) == 0 &&
             r32(fileData + 8) == 0x80010160) {
        // Mode 2 without sync/header: 2336 bytes, sub-header at offset 0
        sectorSize = SECTOR_SIZE_MODE2;
        subHeaderOffset = 0;
        dataOffset = 8;        // after 8-byte sub-header
        LOG("[MoviePlayer] Detected Mode2 2336-byte sector format");
    }
    else if (r32(fileData) == 0x80010160) {
        // Pure data sectors without any headers
        sectorSize = SECTOR_SIZE_DATA;
        subHeaderOffset = 0;
        dataOffset = 0;
        LOG("[MoviePlayer] Detected headerless 2048-byte sector format");
    }
    else {
        LOG("[MoviePlayer] Unknown sector format in %s (first 4 bytes: %02X %02X %02X %02X)",
            path, fileData[0], fileData[1], fileData[2], fileData[3]);
        Close();
        return false;
    }

    // Video chunk payload = user data per sector minus the 32-byte STR chunk header
    videoChunkData = (sectorSize - dataOffset) - VIDEO_CHUNK_HEADER;

    sectorCount = fileSize / sectorSize;
    currentSector = 0;
    finished = false;
    framesDecoded = 0;
    playbackTime = 0.0;
    currentFrameNum = 0xFFFFFFFFu;
    frameReady = false;
    audioStarted = false;

    // Allocate demux buffer (max possible: 20 sectors * payload)
    demuxBufferSize = 20 * videoChunkData;
    demuxBuffer = new u8[demuxBufferSize];
    demuxBytesWritten = 0;

    // Pre-scan first video sector to get dimensions and sector form
    for (u32 s = 0; s < sectorCount && s < 100; s++) {
        const u8* sector = fileData + s * sectorSize;
        SectorHeader sh;
        if (!ParseSectorHeader(sector, sh)) continue;
        if (sh.isVideo) {
            VideoChunkHeader vh;
            if (ParseVideoChunkHeader(sector + dataOffset, vh)) {
                frameWidth = vh.width;
                frameHeight = vh.height;

                // CD-XA Form bit (bit 5 of submode)
                // Form 1 (bit 5 = 0): 2048 bytes user data + EDC/ECC
                // Form 2 (bit 5 = 1): 2328 bytes user data (no EDC/ECC)
                if (sectorSize != SECTOR_SIZE_DATA) {
                    bool isForm2 = (sh.submode & 0x20) != 0;
                    if (!isForm2) {
                        videoChunkData = 2048 - VIDEO_CHUNK_HEADER;
                        LOG("[MoviePlayer] Video sectors are Form 1 (payload %u bytes)", videoChunkData);
                    }
                }
                break;
            }
        }
    }

    if (frameWidth == 0 || frameHeight == 0) {
        LOG("[MoviePlayer] Could not determine frame dimensions");
        Close();
        return false;
    }

    // Allocate RGBA frame buffer
    rgbaBuffer = new u32[frameWidth * frameHeight];
    memset(rgbaBuffer, 0, frameWidth * frameHeight * 4);

    // Pre-decode ALL audio from the file
    // Each audio sector produces up to 4032 samples (18 groups * 8 units * 28 samples)
    audioPCMSize = sectorCount * 4032;
    audioPCM = new s16[audioPCMSize];
    audioPCMWritten = 0;
    memset(prevSample, 0, sizeof(prevSample));

    for (u32 s = 0; s < sectorCount; s++) {
        const u8* sector = fileData + s * sectorSize;
        SectorHeader sh;
        if (!ParseSectorHeader(sector, sh)) continue;
        if (sh.isAudio) {
            DecodeXAAudio(sector + dataOffset, sh.codingInfo);
        }
    }

    // Compute frame rate from sector interleave.
    // PSX CD spins at 2x = 150 sectors/sec. The distance between the first
    // video sector of frame N and the first video sector of frame N+1 gives
    // the total interleaved sectors per displayed video frame.
    {
        u32 firstFrameNum = 0;
        u32 firstFrameStartSector = 0;
        u32 secondFrameStartSector = 0;
        bool foundFirst = false;
        bool foundSecond = false;

        for (u32 s = 0; s < sectorCount && s < 300; s++) {
            const u8* sector = fileData + s * sectorSize;

            SectorHeader sh;
            if (!ParseSectorHeader(sector, sh)) {
                continue;
            }

            if (!sh.isVideo) {
                continue;
            }

            VideoChunkHeader vh;
            if (!ParseVideoChunkHeader(sector + dataOffset, vh)) {
                continue;
            }

            if (!foundFirst) {
                firstFrameNum = vh.frameNum;
                firstFrameStartSector = s;
                foundFirst = true;
                continue;
            }

            if (vh.frameNum != firstFrameNum) {
                secondFrameStartSector = s;
                foundSecond = true;
                break;
            }
        }

        if (foundSecond && secondFrameStartSector > firstFrameStartSector) {
            const u32 sectorsPerFrame = secondFrameStartSector - firstFrameStartSector;

            // 150 sectors/sec for PSX 2x CD speed.
            frameInterval = (f64)sectorsPerFrame / 150.0;
        }
        // else keep the default value from the class.
    }

    LOG("[MoviePlayer] Opened %s: %ux%u, %u sectors (%u bytes/sector, %u payload), %u audio samples, %.1f fps",
        path, frameWidth, frameHeight, sectorCount, sectorSize, videoChunkData, audioPCMWritten,
        1.0 / frameInterval);
    return true;
}

bool MoviePlayer::LoadSubtitles(const char* moviePath) {
    subtitles.clear();
    if (!moviePath || !moviePath[0]) return false;

    std::string subtitlePath(moviePath);
    const size_t extension = subtitlePath.find_last_of('.');
    if (extension == std::string::npos) {
        subtitlePath += ".srt";
    }
    else {
        subtitlePath.replace(extension, std::string::npos, ".srt");
    }

#ifdef MOD_LOADER
    const std::string movieStem = std::filesystem::path(moviePath).stem().string();
    if (const std::string* modSubtitle = ModLoader::Instance().FindSubtitleOverridePath(movieStem.c_str())) {
        subtitlePath = *modSubtitle;
    }
#endif

    u8* subtitleData = nullptr;
    u32 subtitleSize = 0;
    if (!xcReadFileLow(subtitlePath.c_str(), &subtitleData, &subtitleSize)) {
        return false;
    }

    std::string contents(reinterpret_cast<const char*>(subtitleData), subtitleSize);
    delete[] subtitleData;
    if (contents.size() >= 3 && (u8)contents[0] == 0xEF
        && (u8)contents[1] == 0xBB && (u8)contents[2] == 0xBF) {
        contents.erase(0, 3);
    }

    std::stringstream lines(contents);
    std::string line;
    MovieSubtitleEntry current;
    bool readingText = false;
    while (std::getline(lines, line)) {
        line = TrimMovieSubtitleText(line);

        if (!readingText) {
            f64 start = 0.0;
            f64 end = 0.0;
            if (ParseMovieSubtitleTiming(line, start, end)) {
                current = {};
                current.startSeconds = start;
                current.endSeconds = end;
                readingText = true;
            }
            continue;
        }

        if (line.empty()) {
            if (!current.text.empty()) subtitles.push_back(std::move(current));
            current = {};
            readingText = false;
        }
        else {
            if (!current.text.empty()) current.text += '\n';
            current.text += line;
        }
    }

    if (readingText && !current.text.empty()) subtitles.push_back(std::move(current));
    if (!subtitles.empty()) {
        LOG("[MoviePlayer] Loaded %zu subtitles from %s", subtitles.size(), subtitlePath.c_str());
    }
    return !subtitles.empty();
}

void MoviePlayer::RenderSubtitle() const {
    if (subtitles.empty() || !g_textManager) return;

    const f64 elapsed = MovieSyncElapsedSeconds(this);
    const MovieSubtitleEntry* active = nullptr;
    for (const MovieSubtitleEntry& subtitle : subtitles) {
        if (elapsed >= subtitle.startSeconds && elapsed <= subtitle.endSeconds) {
            active = &subtitle;
            break;
        }
    }
    if (!active) return;

    g_textManager->PushState();
    if (!g_textManager->SetFontByName("Menu") && !g_textManager->SetFontByName("Legal")) {
        g_textManager->PopState();
        return;
    }
    g_textManager->SetScale(SCREEN_SCALE_Y(0.40f));
    g_textManager->SetWrapWidth(SCREEN_SCALE_X(900.0f));
    g_textManager->SetLineSpacing(2);
    g_textManager->SetAlignment(TextAlign_Center);
    g_textManager->SetColor(255, 255, 255, 255);
    g_textManager->SetShadow(true, 2.0f, 2.0f, 0, 0, 0, 220);
    g_textManager->SetOutline(true, 1.0f, 0, 0, 0, 255);
    g_textManager->PrintString(active->text.c_str(),
        SCALE_AND_CENTER_X(DEFAULT_SCREEN_WIDTH * 0.5f),
        SCREEN_HEIGHT - SCREEN_SCALE_Y(60.0f));
    g_textManager->PopState();
}

void MoviePlayer::Close() {
    MovieSyncReset(this);
    if (audioVoice) {
        AudioEngine::StopVoice(audioVoice);
        audioVoice = 0;
    }
    audioStarted = false;

    delete[] fileData; fileData = nullptr;
    delete[] demuxBuffer; demuxBuffer = nullptr;
    delete[] rgbaBuffer; rgbaBuffer = nullptr;
    delete[] audioPCM; audioPCM = nullptr;

    if (videoTexture) {
        delete videoTexture;
        videoTexture = nullptr;
    }

#if MODERN_MOVIE_FILTERING
    if (denoiseFBO) {
        denoiseFBO->Release();
        denoiseFBO = nullptr;
    }
    if (upscaleFBO) {
        upscaleFBO->Release();
        upscaleFBO = nullptr;
    }
#endif

    fileSize = 0;
    sectorCount = 0;
    currentSector = 0;
    finished = false;
    frameWidth = 0;
    frameHeight = 0;
    subtitles.clear();
}

// Sector parsing

bool MoviePlayer::ParseSectorHeader(const u8* sector, SectorHeader& hdr) {
    // Sub-header location depends on detected format
    const u8* subHdr = sector + subHeaderOffset;

    if (sectorSize == SECTOR_SIZE_DATA) {
        // No sub-header at all; check if data looks like a STR video chunk
        auto r32 = [](const u8* p) -> u32 { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); };
        u32 magic = r32(sector);
        hdr.submode = 0;
        hdr.channel = 0;
        hdr.codingInfo = 0;
        hdr.isVideo = ((magic & 0x00FFFFFF) == 0x00010160 || magic == 0x80010160);
        hdr.isAudio = false;
        return hdr.isVideo;
    }

    // Raw (2352) or Mode2 (2336): sub-header is 8 bytes
    // Bytes: [0] file, [1] channel, [2] submode, [3] coding info (repeated x2)
    hdr.channel = subHdr[1];
    hdr.submode = subHdr[2];
    hdr.codingInfo = subHdr[3];

    hdr.isAudio = (hdr.submode & 0x04) != 0;

    // Some STR files use submode bit 1 (0x02) for video, others use bit 3 (0x08 = Data).
    // Detect video by checking for STR magic at the data offset as a fallback.
    hdr.isVideo = (hdr.submode & 0x02) != 0;
    if (!hdr.isVideo && !hdr.isAudio) {
        auto r32 = [](const u8* p) -> u32 { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); };
        const u8* userData = sector + dataOffset;
        u32 magic = r32(userData);
        if ((magic & 0x00FFFFFF) == 0x00010160 || magic == 0x80010160) {
            hdr.isVideo = true;
        }
    }
    return true;
}

bool MoviePlayer::ParseVideoChunkHeader(const u8* data, VideoChunkHeader& hdr) {
    // Video sector data starts after CD-XA sub-header (8 bytes repeated = already at data+0)
    // STR Frame Sector Header (32 bytes)
    // Offset 0: u32 magic (usually 0x80010160)
    // Offset 4: u16 chunkNum
    // Offset 6: u16 chunksInFrame
    // Offset 8: u32 frameNum
    // Offset 12: u32 demuxSize
    // Offset 16: u16 width
    // Offset 18: u16 height
    // Offset 20: u16 mdecCodeCount
    // Offset 22: u16 magic3800
    // Offset 24: u16 quantScale
    // Offset 26: u16 version

    auto r16 = [](const u8* p) -> u16 { return p[0] | (p[1] << 8); };
    auto r32 = [](const u8* p) -> u32 { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); };

    u32 magic = r32(data);
    // Most games use 0x80010160, but some variations exist
    if ((magic & 0x00FFFFFF) != 0x00010160 && magic != 0x80010160) {
        return false;
    }

    hdr.chunkNum = r16(data + 4);
    hdr.chunksInFrame = r16(data + 6);
    hdr.frameNum = r32(data + 8);
    hdr.demuxSize = r32(data + 12);
    hdr.width = r16(data + 16);
    hdr.height = r16(data + 18);
    hdr.mdecCodeCount = r16(data + 20);
    hdr.magic3800 = r16(data + 22);
    hdr.quantScale = r16(data + 24);
    hdr.version = r16(data + 26);

    // Sanity check
    if (hdr.width == 0 || hdr.height == 0 || hdr.width > 640 || hdr.height > 480) {
        return false;
    }
    if (hdr.chunksInFrame == 0 || hdr.chunksInFrame > 20) {
        return false;
    }

    return true;
}

// Main frame advance loop

bool MoviePlayer::AdvanceFrame() {
    if (!fileData || finished) {
        return false;
    }

    bool decodedThisCall = false;

    auto UploadDecodedFrameAndStartClock = [&]() {
        if (!videoTexture) {
            videoTexture = new tTexture();
        }

        videoTexture->Create(frameWidth, frameHeight, 32, 8, rgbaBuffer);

#if MODERN_MOVIE_FILTERING
        if (videoTexture->GetTexture()) {
            videoTexture->GetTexture()->SetFilterMode(PDDI_FILTER_BILINEAR);
        }
#endif

        // Start audio only after the first decoded frame has actually been
        // uploaded and can be rendered. This keeps audio from running ahead
        // during slow first-frame decode or resize stalls.
        if (!audioStarted && audioPCMWritten > 0) {
            u32 sampleHandle = AudioEngine::LoadSample(
                audioPCM,
                audioPCMWritten / audioChannels,
                audioSampleRate,
                audioChannels);

            if (sampleHandle != AUDIO_SAMPLE_INVALID) {
                audioVoice = AudioEngine::PlaySample(sampleHandle, 1.0f, 0.0f, false);
                audioStarted = true;

                LOG("[MoviePlayer] Started audio: %u samples, %u Hz, %u ch",
                    audioPCMWritten / audioChannels,
                    audioSampleRate,
                    audioChannels);
            }
        }

        // If the movie has no audio, this still gives video a stable wall-clock
        // timeline. If audio exists, this is started immediately after playback.
        MovieSyncStartIfNeeded(this);
    };

    // Process sectors until we have a complete frame.
    while (currentSector < sectorCount) {
        const u8* sector = fileData + currentSector * sectorSize;
        currentSector++;

        SectorHeader sh;
        if (!ParseSectorHeader(sector, sh)) {
            continue;
        }

        const u8* sectorPayload = sector + dataOffset;

        // Audio is pre-decoded in Open(), so only video is collected here.
        if (sh.isVideo) {
            VideoChunkHeader vh;
            if (!ParseVideoChunkHeader(sectorPayload, vh)) {
                continue;
            }

            // New frame?
            if (vh.frameNum != currentFrameNum) {
                // If a previous frame was completely collected, decode/upload it
                // now and leave this sector to be re-processed on the next call.
                if (frameReady && demuxBytesWritten > 0) {
                    DecodeVideoFrame(demuxBuffer,
                                     demuxBytesWritten,
                                     frameWidth,
                                     frameHeight,
                                     frameQuantScale,
                                     frameVersion);

                    framesDecoded++;
                    decodedThisCall = true;

                    // Reset demux for the next frame.
                    demuxBytesWritten = 0;
                    frameReady = false;

                    // Back up so this sector, the first sector of the new frame,
                    // is consumed by the next AdvanceFrame() call.
                    currentSector--;

                    UploadDecodedFrameAndStartClock();
                    return true;
                }

                // Start collecting the new frame.
                currentFrameNum = vh.frameNum;
                frameWidth = vh.width;
                frameHeight = vh.height;
                frameQuantScale = vh.quantScale;
                frameVersion = vh.version;
                frameDemuxSize = vh.demuxSize;
                frameChunksExpected = vh.chunksInFrame;
                frameChunksReceived = 0;
                demuxBytesWritten = 0;
            }

            // Append chunk data to the demux buffer.
            if (vh.chunkNum < vh.chunksInFrame) {
                const u8* chunkData = sectorPayload + VIDEO_CHUNK_HEADER;
                u32 bytesToCopy = videoChunkData;

                // Do not exceed the frame's actual demux size.
                if (frameDemuxSize > 0 && demuxBytesWritten + bytesToCopy > frameDemuxSize) {
                    bytesToCopy = demuxBytesWritten < frameDemuxSize
                        ? frameDemuxSize - demuxBytesWritten
                        : 0;
                }

                // Do not exceed our allocated demux buffer.
                if (demuxBytesWritten + bytesToCopy > demuxBufferSize) {
                    bytesToCopy = demuxBufferSize - demuxBytesWritten;
                }

                if (bytesToCopy > 0) {
                    memcpy(demuxBuffer + demuxBytesWritten, chunkData, bytesToCopy);
                    demuxBytesWritten += bytesToCopy;
                }

                frameChunksReceived++;

                if (frameChunksReceived >= frameChunksExpected) {
                    frameReady = true;
                }
            }
        }
    }

    // End of file: decode any remaining complete frame.
    if (frameReady && demuxBytesWritten > 0) {
        DecodeVideoFrame(demuxBuffer,
                         demuxBytesWritten,
                         frameWidth,
                         frameHeight,
                         frameQuantScale,
                         frameVersion);

        framesDecoded++;
        decodedThisCall = true;

        demuxBytesWritten = 0;
        frameReady = false;

        UploadDecodedFrameAndStartClock();
    }

    finished = true;
    return decodedThisCall;
}

void MoviePlayer::Render() {
    // Decode the first frame immediately so there is something to show and so
    // audio starts only when the first uploaded texture is ready.
    if (!videoTexture && !finished) {
        AdvanceFrame();
    }

    // Keep video synchronized to the movie clock instead of to render calls.
    // If the window resize stalls rendering, elapsed time jumps forward and this
    // loop catches the video up instead of letting audio drift ahead forever.
    if (videoTexture && MovieSyncIsStarted(this) && frameInterval > 0.0 && !finished) {
        const f64 elapsed = MovieSyncElapsedSeconds(this);

        // framesDecoded is a 1-based count after the first frame is decoded.
        // At t = 0, targetDecodedFrames is 1.
        s32 targetDecodedFrames = (s32)std::floor(elapsed / frameInterval) + 1;
        if (targetDecodedFrames < 1) {
            targetDecodedFrames = 1;
        }

        constexpr s32 kMaxCatchUpFramesPerRender = 12;
        s32 catchUpFrames = 0;

        while (!finished &&
               (s32)framesDecoded < targetDecodedFrames &&
               catchUpFrames < kMaxCatchUpFramesPerRender) {
            if (!AdvanceFrame()) {
                break;
            }

            catchUpFrames++;
        }

        // If the render thread was blocked for a long time, avoid spending a huge
        // amount of time decoding in one render call. The next render calls will
        // continue catching up, and the cap prevents a second visible stall.
        if (catchUpFrames == kMaxCatchUpFramesPerRender) {
            LOG("[MoviePlayer] Video catch-up capped at %d frames", kMaxCatchUpFramesPerRender);
        }
    }

    if (videoTexture) {
#if MODERN_MOVIE_FILTERING
        static pddiBaseShader* s_movieShader = p3d::device ? p3d::device->NewShader("moviesharp") : nullptr;

        if (s_movieShader) {
            if (!denoiseShader)
                denoiseShader = p3d::device->NewShader("moviedenoise");
            if (!upscaleShader)
                upscaleShader = p3d::device->NewShader("movieupscale");
            if (!denoiseFBO && frameWidth > 0 && frameHeight > 0)
                denoiseFBO = p3d::context->CreateRenderTarget(frameWidth, frameHeight, PDDI_RENDER_TARGET_RGBA8);
            if (!upscaleFBO && frameWidth > 0 && frameHeight > 0)
                upscaleFBO = p3d::context->CreateRenderTarget(1024, 768, PDDI_RENDER_TARGET_RGBA8);

            const f32 srcW = (f32)frameWidth;
            const f32 srcH = (f32)frameHeight;
            const f32 upW = 1024.0f;
            const f32 upH = 768.0f;

            if (denoiseShader && denoiseFBO) {
                p3d::context->SetRenderTarget(denoiseFBO);
                denoiseShader->SetTexture(0, videoTexture->GetTexture());
                denoiseShader->SetVector("uTexel", 1.0f / srcW, 1.0f / srcH, 0.0f, 0.0f);
                denoiseShader->SetFloat("uSharpAmount", 0.35f);
                ScreenDraw::DrawShaderQuad(denoiseShader,
                    0.0f, 0.0f, srcW, srcH,
                    0.0f, 1.0f, 1.0f, 0.0f, PDDI_BLEND_NONE,
                    srcW, srcH);
                p3d::context->SetRenderTarget(nullptr);
            }

            if (upscaleShader && upscaleFBO && denoiseFBO) {
                p3d::context->SetRenderTarget(upscaleFBO);
                upscaleShader->SetTexture(0, denoiseFBO->GetTexture());
                upscaleShader->SetVector("uTexel", 1.0f / srcW, 1.0f / srcH, 0.0f, 0.0f);
                ScreenDraw::DrawShaderQuad(upscaleShader,
                    0.0f, 0.0f, upW, upH,
                    0.0f, 1.0f, 1.0f, 0.0f, PDDI_BLEND_NONE,
                    upW, upH);
                p3d::context->SetRenderTarget(nullptr);
            }

            const f32 sourceAspect = srcW / srcH;
            const f32 desiredAspect = 16.0f / 9.0f;

            f32 u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            if (sourceAspect < desiredAspect) {
                const f32 visibleHeight = sourceAspect / desiredAspect;
                const f32 cropY = (1.0f - visibleHeight) * 0.5f;
                v0 = cropY;
                v1 = 1.0f - cropY;
            }
            else if (sourceAspect > desiredAspect) {
                const f32 visibleWidth = desiredAspect / sourceAspect;
                const f32 cropX = (1.0f - visibleWidth) * 0.5f;
                u0 = cropX;
                u1 = 1.0f - cropX;
            }

            f32 x = SCALE_AND_CENTER_X(0.0f);
            f32 y = 0.0f;
            f32 w = SCREEN_SCALE_X(DEFAULT_SCREEN_WIDTH);
            f32 h = SCREEN_HEIGHT;
            const f32 screenAspect = w / h;

            if (screenAspect > desiredAspect) {
                const f32 newH = w / desiredAspect;
                y = (h - newH) * 0.5f;
                h = newH;
            }
            else if (screenAspect < desiredAspect) {
                const f32 newW = h * desiredAspect;
                x = x + (w - newW) * 0.5f;
                w = newW;
            }

            pddiTexture* finalInput = upscaleFBO ? upscaleFBO->GetTexture() : (denoiseFBO ? denoiseFBO->GetTexture() : videoTexture->GetTexture());
            s_movieShader->SetTexture(0, finalInput);
            s_movieShader->SetVector("uTexel", 1.0f / upW, 1.0f / upH, 0.0f, 0.0f);
            s_movieShader->SetFloat("uSharpAmount", 1.5f);

            ScreenDraw::DrawShaderQuad(s_movieShader, x, y, w, h, u0, v0, u1, v1, PDDI_BLEND_NONE);
        }
        else
#endif
        {
            ScreenDraw::DrawFullscreenAdvanced(
                videoTexture,
                frameWidth / (f32)frameHeight,
                16.0f / 9.0f,
                true);
        }
    }

    RenderSubtitle();
}

// BitReader

void MoviePlayer::BitReader::Init(const u8* d, u32 sizeBytes) {
    data = reinterpret_cast<const u16*>(d);
    wordCount = sizeBytes / 2;
    wordPos = 0;
    bitsLeft = 0;
    currentWord = 0;
}

void MoviePlayer::BitReader::NextWord() {
    if (wordPos < wordCount) {
        // Little-endian 16-bit read
        currentWord = data[wordPos++];
    }
    else {
        currentWord = 0;
    }
    bitsLeft = 16;
}

s32 MoviePlayer::BitReader::ReadBits(u32 n) {
    s32 result = 0;
    u32 remaining = n;

    while (remaining > 0) {
        if (bitsLeft == 0) NextWord();

        u32 take = std::min(remaining, bitsLeft);
        // Take 'take' bits from the MSB of currentWord
        u32 shift = bitsLeft - take;
        u32 mask = ((1u << take) - 1) << shift;
        result = (result << take) | ((currentWord & mask) >> shift);
        bitsLeft -= take;
        remaining -= take;
    }

    return result;
}

u32 MoviePlayer::BitReader::PeekBits(u32 n) {
    // Save state
    u32 savedWordPos = wordPos;
    u32 savedBitsLeft = bitsLeft;
    u32 savedWord = currentWord;

    u32 result = (u32)ReadBits(n);

    // Restore state
    wordPos = savedWordPos;
    bitsLeft = savedBitsLeft;
    currentWord = savedWord;

    return result;
}

void MoviePlayer::BitReader::SkipBits(u32 n) {
    ReadBits(n);
}

// MPEG1 AC VLC table
// Run-length VLC entries: { bits_pattern, num_bits, run_of_zeros, ac_coeff_abs }
// The sign bit is separate (appended after the code)
struct VLCEntry {
    u32 code;
    u8  bits;
    u8  run;
    u8  level;
};

// Sorted by bit length for sequential matching
static const VLCEntry AC_VLC_TABLE[] = {
    // 2-bit codes
    { 0b11, 2, 0, 1 },
    // 3-bit codes
    { 0b011, 3, 1, 1 },
    // 4-bit codes
    { 0b0100, 4, 0, 2 },
    { 0b0101, 4, 2, 1 },
    // 5-bit codes
    { 0b00101, 5, 0, 3 },
    { 0b00110, 5, 4, 1 },
    { 0b00111, 5, 3, 1 },
    // 6-bit codes
    { 0b000100, 6, 7, 1 },
    { 0b000101, 6, 6, 1 },
    { 0b000110, 6, 1, 2 },
    { 0b000111, 6, 5, 1 },
    // 7-bit codes
    { 0b0000100, 7, 2, 2 },
    { 0b0000101, 7, 9, 1 },
    { 0b0000110, 7, 0, 4 },
    { 0b0000111, 7, 8, 1 },
    // 8-bit codes
    { 0b00100000, 8, 13, 1 },
    { 0b00100001, 8, 0, 6 },
    { 0b00100010, 8, 12, 1 },
    { 0b00100011, 8, 11, 1 },
    { 0b00100100, 8, 3, 2 },
    { 0b00100101, 8, 1, 3 },
    { 0b00100110, 8, 0, 5 },
    { 0b00100111, 8, 10, 1 },
    // 10-bit codes
    { 0b0000001000, 10, 16, 1 },
    { 0b0000001001, 10, 5, 2 },
    { 0b0000001010, 10, 0, 7 },
    { 0b0000001011, 10, 2, 3 },
    { 0b0000001100, 10, 1, 4 },
    { 0b0000001101, 10, 15, 1 },
    { 0b0000001110, 10, 14, 1 },
    { 0b0000001111, 10, 4, 2 },
    // 12-bit codes
    { 0b000000010000, 12, 0, 11 },
    { 0b000000010001, 12, 8, 2 },
    { 0b000000010010, 12, 4, 3 },
    { 0b000000010011, 12, 0, 10 },
    { 0b000000010100, 12, 2, 4 },
    { 0b000000010101, 12, 7, 2 },
    { 0b000000010110, 12, 21, 1 },
    { 0b000000010111, 12, 20, 1 },
    { 0b000000011000, 12, 0, 9 },
    { 0b000000011001, 12, 19, 1 },
    { 0b000000011010, 12, 18, 1 },
    { 0b000000011011, 12, 1, 5 },
    { 0b000000011100, 12, 3, 3 },
    { 0b000000011101, 12, 0, 8 },
    { 0b000000011110, 12, 6, 2 },
    { 0b000000011111, 12, 17, 1 },
    // 13-bit codes
    { 0b0000000010000, 13, 10, 2 },
    { 0b0000000010001, 13, 9, 2 },
    { 0b0000000010010, 13, 5, 3 },
    { 0b0000000010011, 13, 3, 4 },
    { 0b0000000010100, 13, 2, 5 },
    { 0b0000000010101, 13, 1, 7 },
    { 0b0000000010110, 13, 1, 6 },
    { 0b0000000010111, 13, 0, 15 },
    { 0b0000000011000, 13, 0, 14 },
    { 0b0000000011001, 13, 0, 13 },
    { 0b0000000011010, 13, 0, 12 },
    { 0b0000000011011, 13, 26, 1 },
    { 0b0000000011100, 13, 25, 1 },
    { 0b0000000011101, 13, 24, 1 },
    { 0b0000000011110, 13, 23, 1 },
    { 0b0000000011111, 13, 22, 1 },
    // 14-bit codes
    { 0b00000000010000, 14, 0, 31 },
    { 0b00000000010001, 14, 0, 30 },
    { 0b00000000010010, 14, 0, 29 },
    { 0b00000000010011, 14, 0, 28 },
    { 0b00000000010100, 14, 0, 27 },
    { 0b00000000010101, 14, 0, 26 },
    { 0b00000000010110, 14, 0, 25 },
    { 0b00000000010111, 14, 0, 24 },
    { 0b00000000011000, 14, 0, 23 },
    { 0b00000000011001, 14, 0, 22 },
    { 0b00000000011010, 14, 0, 21 },
    { 0b00000000011011, 14, 0, 20 },
    { 0b00000000011100, 14, 0, 19 },
    { 0b00000000011101, 14, 0, 18 },
    { 0b00000000011110, 14, 0, 17 },
    { 0b00000000011111, 14, 0, 16 },
    // 15-bit codes
    { 0b000000000010000, 15, 0, 40 },
    { 0b000000000010001, 15, 0, 39 },
    { 0b000000000010010, 15, 0, 38 },
    { 0b000000000010011, 15, 0, 37 },
    { 0b000000000010100, 15, 0, 36 },
    { 0b000000000010101, 15, 0, 35 },
    { 0b000000000010110, 15, 0, 34 },
    { 0b000000000010111, 15, 0, 33 },
    { 0b000000000011000, 15, 0, 32 },
    { 0b000000000011001, 15, 1, 14 },
    { 0b000000000011010, 15, 1, 13 },
    { 0b000000000011011, 15, 1, 12 },
    { 0b000000000011100, 15, 1, 11 },
    { 0b000000000011101, 15, 1, 10 },
    { 0b000000000011110, 15, 1, 9 },
    { 0b000000000011111, 15, 1, 8 },
    // 16-bit codes
    { 0b0000000000010000, 16, 1, 18 },
    { 0b0000000000010001, 16, 1, 17 },
    { 0b0000000000010010, 16, 1, 16 },
    { 0b0000000000010011, 16, 1, 15 },
    { 0b0000000000010100, 16, 6, 3 },
    { 0b0000000000010101, 16, 16, 2 },
    { 0b0000000000010110, 16, 15, 2 },
    { 0b0000000000010111, 16, 14, 2 },
    { 0b0000000000011000, 16, 13, 2 },
    { 0b0000000000011001, 16, 12, 2 },
    { 0b0000000000011010, 16, 11, 2 },
    { 0b0000000000011011, 16, 31, 1 },
    { 0b0000000000011100, 16, 30, 1 },
    { 0b0000000000011101, 16, 29, 1 },
    { 0b0000000000011110, 16, 28, 1 },
    { 0b0000000000011111, 16, 27, 1 },
};
static constexpr u32 AC_VLC_COUNT = sizeof(AC_VLC_TABLE) / sizeof(AC_VLC_TABLE[0]);

void MoviePlayer::DecodeVideoFrame(const u8* demuxData, u32 demuxSize,
                                   u16 width, u16 height,
                                   u16 quantScale, u16 version) {
    if (!demuxData || demuxSize < 8) return;

    auto r16 = [](const u8* p) -> u16 { return p[0] | (p[1] << 8); };
    quantScale = r16(demuxData + 4);
    version = r16(demuxData + 6);


    BitReader br;
    br.Init(demuxData + 8, demuxSize - 8);

    u32 mbW = (width + 15) / 16;
    u32 mbH = (height + 15) / 16;

    s32 prevDC_Cr = 0, prevDC_Cb = 0, prevDC_Y = 0;

    // PSX MDEC macroblock order is column-major
    for (u32 mbX = 0; mbX < mbW; mbX++) {
        for (u32 mbY = 0; mbY < mbH; mbY++) {
            // 6 blocks per macroblock: Cr, Cb, Y1, Y2, Y3, Y4
            // Each block is 64 s32 values in row-major 8x8 matrix order
            s32 cr_block[64], cb_block[64];
            s32 y1_block[64], y2_block[64], y3_block[64], y4_block[64];
            s32 cr_out[64], cb_out[64];
            s32 y1_out[64], y2_out[64], y3_out[64], y4_out[64];

            DecodeBlock(br, cr_block, quantScale, version, prevDC_Cr, true, true);
            IDCT(cr_block, cr_out);

            DecodeBlock(br, cb_block, quantScale, version, prevDC_Cb, true, false);
            IDCT(cb_block, cb_out);

            DecodeBlock(br, y1_block, quantScale, version, prevDC_Y, false, false);
            IDCT(y1_block, y1_out);

            DecodeBlock(br, y2_block, quantScale, version, prevDC_Y, false, false);
            IDCT(y2_block, y2_out);

            DecodeBlock(br, y3_block, quantScale, version, prevDC_Y, false, false);
            IDCT(y3_block, y3_out);

            DecodeBlock(br, y4_block, quantScale, version, prevDC_Y, false, false);
            IDCT(y4_block, y4_out);

            u32 px = mbX * 16;
            u32 py = mbY * 16;
            if (px + 16 <= width && py + 16 <= height) {
                MacroblockToRGB(cr_out, cb_out, y1_out, y2_out, y3_out, y4_out,
                                rgbaBuffer + py * width + px, width);
            }
            else {
                u32 temp[16 * 16];
                MacroblockToRGB(cr_out, cb_out, y1_out, y2_out, y3_out, y4_out,
                                temp, 16);
                for (u32 dy = 0; dy < 16 && py + dy < height; dy++) {
                    for (u32 dx = 0; dx < 16 && px + dx < width; dx++) {
                        rgbaBuffer[(py + dy) * width + (px + dx)] = temp[dy * 16 + dx];
                    }
                }
            }
        }
    }

}

// Bitstream block decoding
// Combined VLC decode + reverse-zigzag + dequantize (matches jpsxdec architecture)

// Quantization table as flat array (row-major) for indexing by matrix position
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

// Reverse zig-zag lookup: vectorPosition -> matrix linear index (row*8+col)
// Matches jpsxdec MdecInputStream.REVERSE_ZIG_ZAG_LOOKUP_LIST
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

void MoviePlayer::DecodeBlock(BitReader& br, s32 block[64],
                              u16 quantScale, u16 version,
                              s32& prevDC, bool isChroma, bool isCr) {
    std::memset(block, 0, 64 * sizeof(s32));

    // Read DC coefficient
    s32 dc;
    if (version == 2) {
        // V2: 10 bits signed, absolute
        dc = SignExtend(br.ReadBits(10), 10);
    }
    else {
        // V3: differential Huffman-coded DC (MPEG1 ISO 11172 tables)
        u32 peek = br.PeekBits(8);
        s32 dcDiff = 0;
        u32 sizeCode = 0;

        if (isChroma) {
            // Chroma DC VLC (table 2-D.12 from ISO 11172-2)
            if ((peek >> 0) == 0xFE) {
                br.SkipBits(8); sizeCode = 8;
            }
            else if ((peek >> 1) >= 0x7E) {
                br.SkipBits(7); sizeCode = 7;
            }
            else if ((peek >> 2) >= 0x3E) {
                br.SkipBits(6); sizeCode = 6;
            }
            else if ((peek >> 3) >= 0x1E) {
                br.SkipBits(5); sizeCode = 5;
            }
            else if ((peek >> 4) >= 0x0E) {
                br.SkipBits(4); sizeCode = 4;
            }
            else if ((peek >> 5) >= 0x06) {
                br.SkipBits(3); sizeCode = 3;
            }
            else if ((peek >> 6) >= 0x02) {
                br.SkipBits(2); sizeCode = 2;
            }
            else if ((peek >> 6) == 0x01) {
                br.SkipBits(2); sizeCode = 1;
            }
            else {
                br.SkipBits(2); sizeCode = 0;
            }
        }
        else {
            // Luma DC VLC (table 2-D.12 from ISO 11172-2)
            if ((peek >> 1) >= 0x7E) {
                br.SkipBits(7); sizeCode = 8;
            }
            else if ((peek >> 2) >= 0x3E) {
                br.SkipBits(6); sizeCode = 7;
            }
            else if ((peek >> 3) >= 0x1E) {
                br.SkipBits(5); sizeCode = 6;
            }
            else if ((peek >> 4) >= 0x0E) {
                br.SkipBits(4); sizeCode = 5;
            }
            else if ((peek >> 5) >= 0x06) {
                br.SkipBits(3); sizeCode = 4;
            }
            else if ((peek >> 5) == 0x05) {
                br.SkipBits(3); sizeCode = 3;
            }
            else if ((peek >> 6) == 0x01) {
                br.SkipBits(2); sizeCode = 2;
            }
            else if ((peek >> 6) == 0x00) {
                br.SkipBits(2); sizeCode = 1;
            }
            else {
                br.SkipBits(3); sizeCode = 0;
            }
        }

        if (sizeCode == 0) {
            dcDiff = 0;
        }
        else {
            s32 valueBits = br.ReadBits(sizeCode);
            // Top bit 0 = negative, top bit 1 = positive (jpsxdec convention)
            if (valueBits & (1 << (sizeCode - 1))) {
                dcDiff = valueBits;
            }
            else {
                dcDiff = valueBits - ((1 << sizeCode) - 1);
            }
        }

        dcDiff *= 4; // Scale up to 10-bit precision
        dc = prevDC + dcDiff;
        prevDC = dc;
    }

    // Dequantize DC: dc * quantTable[0] (jpsxdec: bottom10 * quantTable[0])
    if (dc != 0) {
        block[0] = dc * PSX_QUANT_FLAT[0];
    }

    // Read AC coefficients using VLC table, with combined reverse-zigzag + dequantize
    u32 vectorPos = 0;
    while (vectorPos < 63) {
        // Check for END_OF_BLOCK: binary "10"
        if (br.PeekBits(2) == 0x2) {
            br.SkipBits(2);
            break;
        }

        s32 run = 0;
        s32 level = 0;

        // Check for escape code: "000001"
        if (br.PeekBits(6) == 0x01) {
            br.SkipBits(6);
            run = (s32)br.ReadBits(6);
            level = SignExtend(br.ReadBits(10), 10);
        }
        else {
            // Try VLC table match
            bool matched = false;
            for (u32 v = 0; v < AC_VLC_COUNT; v++) {
                const VLCEntry& e = AC_VLC_TABLE[v];
                u32 peeked = br.PeekBits(e.bits + 1);
                u32 codeWithoutSign = peeked >> 1;
                if (codeWithoutSign == e.code) {
                    br.SkipBits(e.bits + 1);
                    level = e.level;
                    if (peeked & 1) level = -level;
                    run = e.run;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                br.SkipBits(1);
                break;
            }
        }

        // Advance vector position by run+1 (jpsxdec: vectorPos += top6 + 1)
        vectorPos += run + 1;
        if (vectorPos >= 64) break;

        // Reverse zigzag to get matrix position, then dequantize
        // jpsxdec: (bottom10 * quantTable[matrixPos] * qscale + 4) >> 3
        if (level != 0) {
            u32 matrixPos = REVERSE_ZIG_ZAG[vectorPos];
            block[matrixPos] = (level * (s32)PSX_QUANT_FLAT[matrixPos] * (s32)quantScale + 4) >> 3;
        }
    }
}

// IDCT matching jpsxdec PsxMdecIDCT_int
// Full 64-bit precision intermediate, single >>32 at end.
// Input/output are flat 64-element arrays in row-major order.

void MoviePlayer::IDCT(const s32 block[64], s32 out[64]) {
    s64 temp[64];

    // Pass 1: temp[x + y*8] = sum_i block[x + i*8] * COSINE[y + i*8]
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            s64 sum = 0;
            for (int i = 0; i < 8; i++) {
                sum += (s64)block[x + i * 8] * IDCT_MATRIX[i][y];
            }
            temp[x + y * 8] = sum;
        }
    }

    // Pass 2: out[x + y*8] = shrRound(sum_i COSINE[x + i*8] * temp[i + y*8], 32)
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            s64 sum = 0;
            for (int i = 0; i < 8; i++) {
                sum += IDCT_MATRIX[i][x] * temp[i + y * 8];
            }
            // jpsxdec shrRound: (val + (1 << (bits-1))) >> bits
            out[x + y * 8] = (s32)((sum + (1LL << 31)) >> 32);
        }
    }
}

// Convert YCbCr macroblock to RGBA

// jpsxdec PsxYCbCr_int fixed-point constants (16-bit precision)
// These are tuned to match PSX MDEC hardware output exactly.
static constexpr s64 JPSXDEC_1_402 = 91893;  // round(1.402 * 65536) + 12
static constexpr s64 JPSXDEC_0_3437 = 22525;  // round(0.3437 * 65536)
static constexpr s64 JPSXDEC_0_7143 = 46812;  // round(0.7143 * 65536)
static constexpr s64 JPSXDEC_1_772 = 116224; // round(1.772 * 65536) + 94

// jpsxdec shrRound for YCbCr
static inline s32 ShrRound(s64 val, int bits) {
    return (s32)((val + (1LL << (bits - 1))) >> bits);
}

#if MODERN_MOVIE_FILTERING
static inline s32 SampleChromaBilinear(const s32 plane[64], f32 fx, f32 fy) {
    s32 x0 = (s32)std::floor(fx);
    s32 y0 = (s32)std::floor(fy);
    f32 tx = fx - (f32)x0;
    f32 ty = fy - (f32)y0;
    s32 x1 = Clamp(x0 + 1, 0, 7);
    s32 y1 = Clamp(y0 + 1, 0, 7);
    x0 = Clamp(x0, 0, 7);
    y0 = Clamp(y0, 0, 7);

    f32 c00 = (f32)plane[y0 * 8 + x0];
    f32 c10 = (f32)plane[y0 * 8 + x1];
    f32 c01 = (f32)plane[y1 * 8 + x0];
    f32 c11 = (f32)plane[y1 * 8 + x1];

    f32 top = c00 + (c10 - c00) * tx;
    f32 bot = c01 + (c11 - c01) * tx;
    return (s32)std::lround(top + (bot - top) * ty);
}
#endif

void MoviePlayer::MacroblockToRGB(const s32 cr[64], const s32 cb[64],
                                  const s32 y1[64], const s32 y2[64],
                                  const s32 y3[64], const s32 y4[64],
                                  u32* rgbaOut, u32 stride) {
    for (int py = 0; py < 16; py++) {
        for (int px = 0; px < 16; px++) {
            // Get Y value from appropriate sub-block (flat indexing: row*8+col)
            s32 Y;
            int bx = px % 8, by = py % 8;
            int blockIdx = by * 8 + bx;
            if (px < 8 && py < 8) Y = y1[blockIdx];
            else if (px >= 8 && py < 8) Y = y2[blockIdx];
            else if (px < 8 && py >= 8) Y = y3[blockIdx];
            else Y = y4[blockIdx];

            // Chroma is subsampled 2:1 in both directions
#if MODERN_MOVIE_FILTERING
            f32 cfx = (px - 0.5f) * 0.5f;
            f32 cfy = (py - 0.5f) * 0.5f;
            s32 Cr = SampleChromaBilinear(cr, cfx, cfy);
            s32 Cb = SampleChromaBilinear(cb, cfx, cfy);
#else
            int chromaIdx = (py / 2) * 8 + (px / 2);
            s32 Cr = cr[chromaIdx];
            s32 Cb = cb[chromaIdx];
#endif

            // PSX YCbCr -> RGB conversion (jpsxdec PsxYCbCr_int formula)
            // Y is centered at 0, add 128 for display range
            s32 Yshift = Y + 128;
            s32 r = Yshift + ShrRound(JPSXDEC_1_402 * Cr, 16);
            s32 g = Yshift + ShrRound(-JPSXDEC_0_3437 * Cb - JPSXDEC_0_7143 * Cr, 16);
            s32 b = Yshift + ShrRound(JPSXDEC_1_772 * Cb, 16);

            r = Clamp(r, 0, 255);
            g = Clamp(g, 0, 255);
            b = Clamp(b, 0, 255);

            rgbaOut[py * stride + px] = (255u << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
        }
    }
}

// XA ADPCM audio decoding

void MoviePlayer::DecodeXAAudio(const u8* sectorData, u8 codingInfo) {
    // CD-XA audio format from coding info byte:
    // bit 0: stereo (0=mono, 1=stereo)
    // bit 2: sample rate (0=37800, 1=18900)
    // bit 4: bits per sample (0=4bpp mode B/C, 1=8bpp mode A)

    bool stereo = (codingInfo & 0x01) != 0;
    bool halfRate = (codingInfo & 0x04) != 0;
    bool eightBit = (codingInfo & 0x10) != 0;

    audioChannels = stereo ? 2 : 1;
    audioSampleRate = halfRate ? 18900 : 37800;

    if (eightBit) return; // Mode A (8bpp) not commonly used in games

    // XA ADPCM: 18 sound groups of 128 bytes each = 2304 bytes total
    // Each sound group: 16 bytes of sound parameters + 112 bytes of sound data
    // For 4bpp: 8 sound units per sound group, generating 28 samples each
    // Total: 18 groups * 8 units * 28 samples = 4032 samples (mono) or 2016 per channel (stereo)

    const u8* audioData = sectorData;

    for (u32 group = 0; group < 18; group++) {
        const u8* groupData = audioData + group * 128;

        // Sound parameters: first 16 bytes contain params for 8 sound units
        // Sound data: next 112 bytes (28 words of 4 bytes each)

        // For stereo, decode in unit pairs and interleave L/R
        // Temp buffers for one unit's 28 samples
        s16 unitSamples[8][28];

        for (u32 unit = 0; unit < 8; unit++) {
            // Sound parameter bytes:
            // Bytes 0-3: range/filter for units 0-3
            // Bytes 4-7: copy of bytes 0-3
            // Bytes 8-11: range/filter for units 4-7
            // Bytes 12-15: copy of bytes 8-11
            u8 filterParam;
            if (unit < 4) {
                filterParam = groupData[unit];       // bytes 0,1,2,3
            }
            else {
                filterParam = groupData[unit + 4];   // bytes 8,9,10,11
            }

            u32 range = filterParam & 0x0F;
            u32 filter = (filterParam >> 4) & 0x03;
            s32 k0 = XA_K0[filter];
            s32 k1 = XA_K1[filter];

            u32 ch = stereo ? (unit & 1) : 0;

            // Decode 28 samples from the sound data portion
            for (u32 s = 0; s < 28; s++) {
                u8 dataByte = groupData[16 + s * 4 + (unit / 2)];
                s32 nibble;
                if (unit & 1) {
                    nibble = (dataByte >> 4) & 0x0F;
                }
                else {
                    nibble = dataByte & 0x0F;
                }

                // Sign-extend nibble to s32
                if (nibble >= 8) nibble -= 16;

                // Decode ADPCM
                s32 sample = (nibble << (12 - range));
                sample += (k0 * (s32)prevSample[ch][0] + k1 * (s32)prevSample[ch][1]) / 64;
                sample = Clamp(sample, -32768, 32767);

                prevSample[ch][1] = prevSample[ch][0];
                prevSample[ch][0] = (f64)sample;

                unitSamples[unit][s] = (s16)sample;
            }
        }

        // Write decoded samples to PCM buffer
        if (stereo) {
            // Interleave L/R: unit pairs (0,1), (2,3), (4,5), (6,7)
            for (u32 pair = 0; pair < 4; pair++) {
                u32 lUnit = pair * 2;
                u32 rUnit = pair * 2 + 1;
                for (u32 s = 0; s < 28; s++) {
                    if (audioPCMWritten + 1 < audioPCMSize) {
                        audioPCM[audioPCMWritten++] = unitSamples[lUnit][s];
                        audioPCM[audioPCMWritten++] = unitSamples[rUnit][s];
                    }
                }
            }
        }
        else {
            // Mono: write all units sequentially
            for (u32 unit = 0; unit < 8; unit++) {
                for (u32 s = 0; s < 28; s++) {
                    if (audioPCMWritten < audioPCMSize) {
                        audioPCM[audioPCMWritten++] = unitSamples[unit][s];
                    }
                }
            }
        }
    }
}
