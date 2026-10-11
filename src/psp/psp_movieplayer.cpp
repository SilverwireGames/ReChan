// PSP MoviePlayer: plays the original .STR movies from the Memory Stick.
//
// The desktop player loads a whole movie and pre-decodes all of its audio,
// tens of megabytes for the longer FMVs. Here a reader thread streams the
// file into a ring of raw sectors and two cursors walk that ring: the audio
// cursor decodes XA sectors ahead into a PCM ring that pspaudiolib channel 1
// plays (channel 0 is the game mixer), and the video cursor decodes the MDEC
// frame that the audio clock says is due, straight into an RGB565 texture.
// Late frames are skipped without decoding: every MDEC frame stands alone.
#include "radmovie/movieplayer.h"

#include "extra/customtext.h"
#include "gen/common.h"
#include "p3d/fileio.h"
#include "p3d/texture.h"
#include "pc/log.h"
#include "pc/textmgr.h"
#include "pc/tim.h"
#include "pddi/psp/psprender.h"
#include "psp/psp_strdecode.h"

#include <pspaudiolib.h>
#include <pspkernel.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <string>
#include <vector>

namespace {

// Raw sector ring: 384 Mode 2 sectors (~900 KB, ~2.5 s of a 2x CD stream),
// filled 16 sectors per Memory Stick read.
constexpr u32 kRingSectors = 384;
constexpr u32 kReadSectors = 16;
static_assert(kRingSectors % kReadSectors == 0, "reads never wrap the ring");
// The reader blocks in sceIoRead or sleeps; it only needs to beat the game
// thread (0x20) to the CPU when a read completes.
constexpr int kReaderPriority = 0x1E;
constexpr int kReaderStackSize = 0x4000;

// Decoded audio, as stereo frames: 16384 frames is 0.87 s at 18900 Hz.
constexpr u32 kPcmFrames = 16384;
constexpr u32 kPcmMask = kPcmFrames - 1;
// Audio starts once this much is decoded, so the first callbacks never run dry.
constexpr u32 kPcmStartFrames = 4096;
constexpr u32 kOutputRate = 44100;
constexpr int kMovieAudioChannel = 1;

constexpr u32 kMaxFrameChunks = 20;
constexpr u32 kVideoChunkHeader = 32;

constexpr u32 SectorSizeRaw = 2352;
constexpr u32 SectorSizeMode2 = 2336;
constexpr u32 SectorSizeData = 2048;

inline u16 ReadU16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline u32 ReadU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16)
        | (static_cast<u32>(p[3]) << 24);
}
inline bool IsStrMagic(u32 magic) { return (magic & 0x00FFFFFFu) == 0x00010160u; }

u64 NowMicros() { return sceKernelGetSystemTimeWide(); }

struct Subtitle {
    f32 start = 0.0f;
    f32 end = 0.0f;
    std::string text;
};

std::string Trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

bool ParseSrtTime(const std::string& value, f32& seconds) {
    unsigned h = 0, m = 0, s = 0, ms = 0;
    if (std::sscanf(value.c_str(), "%u:%u:%u,%u", &h, &m, &s, &ms) != 4) return false;
    seconds = static_cast<f32>(h * 3600u + m * 60u + s) + static_cast<f32>(ms) / 1000.0f;
    return true;
}

// SubRip cues: "hh:mm:ss,mmm --> hh:mm:ss,mmm" followed by text lines.
std::vector<Subtitle> ParseSrt(const std::string& contents) {
    std::vector<Subtitle> cues;
    Subtitle current;
    bool readingText = false;
    size_t pos = (contents.size() >= 3 && static_cast<u8>(contents[0]) == 0xEF) ? 3 : 0;
    while (pos <= contents.size()) {
        size_t end = contents.find('\n', pos);
        if (end == std::string::npos) end = contents.size();
        const std::string line = Trim(contents.substr(pos, end - pos));
        pos = end + 1;
        if (!readingText) {
            const size_t arrow = line.find("-->");
            if (arrow != std::string::npos && ParseSrtTime(Trim(line.substr(0, arrow)), current.start)
                && ParseSrtTime(Trim(line.substr(arrow + 3)), current.end)) {
                current.text.clear();
                readingText = true;
            }
        }
        else if (line.empty()) {
            if (!current.text.empty()) cues.push_back(current);
            readingText = false;
        }
        else {
            if (!current.text.empty()) current.text += '\n';
            current.text += line;
        }
        if (end == contents.size()) break;
    }
    if (readingText && !current.text.empty()) cues.push_back(current);
    return cues;
}

const char* LanguageCode() {
    switch (g_customText.GetLanguage()) {
        case LangGerman: return "de";
        case LangFrench: return "fr";
        case LangItalian: return "it";
        case LangSpanish: return "es";
        case LangPortuguese: return "pt";
        default: return "en";
    }
}

} // namespace

struct MoviePlayer::Impl {
    // Sector layout, detected at Open.
    u32 sectorSize = 0;
    u32 subHeaderOffset = 0;
    u32 dataOffset = 0;

    // Reader thread and ring. The reader owns `written`, the consumer
    // `released`; sectors in [released, written) are valid.
    p3d::io::FileHandle file;
    u8* ring = nullptr;
    SceUID readerThread = -1;
    std::atomic<u32> written{ 0 };
    std::atomic<u32> released{ 0 };
    std::atomic<bool> readerEof{ false };
    std::atomic<bool> stopReader{ false };

    // Audio: XA sectors decoded ahead into the PCM ring; the channel 1
    // callback resamples to 44.1 kHz. Its consumed-frame count is the clock.
    u32 audioCursor = 0;
    StrDecode::XaState xa;
    s16* xaScratch = nullptr;
    u32* pcm = nullptr; // stereo frames, left in the low half
    std::atomic<u32> pcmWritten{ 0 };
    std::atomic<u32> pcmRead{ 0 };
    u32 pcmFraction = 0; // callback-owned 16.16 position past pcmRead
    u32 audioRate = 0;
    u32 resampleStep = 0;
    bool audioSeen = false;
    std::atomic<bool> audioRunning{ false };
    bool audioDone = false;

    // Clock: audio position while audio plays, then wall time.
    bool clockStarted = false;
    u64 wallStartUs = 0;
    f32 wallBase = 0.0f;
    bool onWallClock = false;

    // Video.
    u32 videoCursor = 0;
    u32 width = 0;
    u32 height = 0;
    u32 firstFrameNum = 0;
    bool firstFrameSeen = false;
    u32 firstFrameSector = 0;
    f32 frameInterval = 1.0f / 15.0f;
    bool intervalKnown = false;
    s32 shownFrame = -1;
    bool videoDone = false;
    u8* demux = nullptr;
    u32 demuxCapacity = 0;
    tTexture* texture = nullptr;

    std::vector<Subtitle> subtitles;
    bool finished = false;
    std::atomic<bool> pcmFinal{ false }; // stream decoded: a dry ring is the end, not an underrun

#if defined(PSP_MOVIE_QA)
    // Movie QA report (MOVIE_<name>.TXT), written when the player closes.
    std::string qaName;
    u64 qaOpenUs = 0;
    u32 qaDecoded = 0;
    u32 qaSkipped = 0;
    u32 qaDecodeUsMax = 0;
    u64 qaDecodeUsTotal = 0;
    f32 qaLateMax = 0.0f;
    u32 qaPcmMin = 0xFFFFFFFFu;
    std::atomic<u32> qaUnderruns{ 0 };
    std::atomic<u32> qaReadUsMax{ 0 };
    void WriteQaReport() const;
#endif

    const u8* Sector(u32 index) const { return ring + (index % kRingSectors) * sectorSize; }

    bool ParseSector(const u8* sector, u8& submode, u8& coding, bool& video, bool& audio) const {
        const u8* user = sector + dataOffset;
        if (sectorSize == SectorSizeData) {
            submode = 0;
            coding = 0;
            audio = false;
            video = IsStrMagic(ReadU32(user));
            return video;
        }
        const u8* sub = sector + subHeaderOffset;
        submode = sub[2];
        coding = sub[3];
        audio = (submode & 0x04) != 0;
        video = !audio && IsStrMagic(ReadU32(user));
        return true;
    }

    f32 Clock() const;
    void PumpAudio();
    void StartAudioIfReady();
    void UpdateVideo();
    bool FindNextFrame(u32 from, u32& frameIndex, u32& end, bool& complete);
    bool DecodeFrameAt(u32 start, u32 end);
    void Shutdown();
};

namespace {

int ReaderEntry(SceSize, void* argp) {
    MoviePlayer::Impl* m = *static_cast<MoviePlayer::Impl**>(argp);
    const u32 chunkBytes = kReadSectors * m->sectorSize;
    while (!m->stopReader.load(std::memory_order_acquire)) {
        const u32 written = m->written.load(std::memory_order_relaxed);
        if (written - m->released.load(std::memory_order_acquire) + kReadSectors > kRingSectors) {
            sceKernelDelayThread(4000);
            continue;
        }
        u8* dst = m->ring + (written % kRingSectors) * m->sectorSize;
#if defined(PSP_MOVIE_QA)
        const u32 readStart = sceKernelGetSystemTimeLow();
#endif
        const size_t got = m->file.Read(dst, chunkBytes);
#if defined(PSP_MOVIE_QA)
        const u32 readUs = sceKernelGetSystemTimeLow() - readStart;
        if (readUs > m->qaReadUsMax.load(std::memory_order_relaxed)) m->qaReadUsMax.store(readUs, std::memory_order_relaxed);
#endif
        const u32 sectors = static_cast<u32>(got / m->sectorSize);
        m->written.store(written + sectors, std::memory_order_release);
        if (sectors < kReadSectors) break;
    }
    m->readerEof.store(true, std::memory_order_release);
    return 0;
}

// pspaudiolib channel 1: resamples the PCM ring to 44.1 kHz stereo with
// linear interpolation. When the ring runs dry it plays silence and the
// clock waits, so video stays on the sound.
void AudioCallback(void* buffer, unsigned int frames, void* userdata) {
    auto* m = static_cast<MoviePlayer::Impl*>(userdata);
    u32* out = static_cast<u32*>(buffer);
    if (!m || !m->audioRunning.load(std::memory_order_acquire)) {
        std::memset(out, 0, frames * sizeof(u32));
        return;
    }
    const u32 available = m->pcmWritten.load(std::memory_order_acquire);
    u32 read = m->pcmRead.load(std::memory_order_relaxed);
    u32 fraction = m->pcmFraction;
    const u32 step = m->resampleStep;
    unsigned int i = 0;
    for (; i < frames; ++i) {
        if (read + 1 >= available) break;
        const u32 a = m->pcm[read & kPcmMask];
        const u32 b = m->pcm[(read + 1) & kPcmMask];
        const s32 t = static_cast<s32>(fraction);
        const s32 la = static_cast<s16>(a & 0xFFFF), ra = static_cast<s16>(a >> 16);
        const s32 lb = static_cast<s16>(b & 0xFFFF), rb = static_cast<s16>(b >> 16);
        const s32 l = la + (((lb - la) * t) >> 16);
        const s32 r = ra + (((rb - ra) * t) >> 16);
        out[i] = static_cast<u32>(static_cast<u16>(l)) | (static_cast<u32>(static_cast<u16>(r)) << 16);
        fraction += step;
        read += fraction >> 16;
        fraction &= 0xFFFF;
    }
#if defined(PSP_MOVIE_QA)
    if (i < frames && !m->pcmFinal.load(std::memory_order_acquire)) {
        m->qaUnderruns.store(m->qaUnderruns.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
#endif
    for (; i < frames; ++i) out[i] = 0;
    m->pcmFraction = fraction;
    m->pcmRead.store(read, std::memory_order_release);
}

} // namespace

f32 MoviePlayer::Impl::Clock() const {
    if (!clockStarted) return 0.0f;
    if (onWallClock) return wallBase + static_cast<f32>(NowMicros() - wallStartUs) / 1000000.0f;
    return static_cast<f32>(pcmRead.load(std::memory_order_acquire)) / static_cast<f32>(audioRate);
}

// Decodes audio sectors ahead of the audio cursor while the PCM ring has
// room. Video sectors in between stay in the sector ring for the video
// cursor.
void MoviePlayer::Impl::PumpAudio() {
    const u32 available = written.load(std::memory_order_acquire);
    while (audioCursor < available) {
        const u32 room = kPcmFrames - (pcmWritten.load(std::memory_order_relaxed)
            - pcmRead.load(std::memory_order_acquire));
        if (room < StrDecode::kXaSamplesPerSector) break;
        const u8* sector = Sector(audioCursor);
        u8 submode = 0, coding = 0;
        bool video = false, audio = false;
        if (ParseSector(sector, submode, coding, video, audio) && audio) {
            const u32 rate = StrDecode::XaSampleRate(coding);
            if (!audioSeen) {
                audioSeen = true;
                audioRate = rate;
                resampleStep = (rate << 16) / kOutputRate;
            }
            if (rate == audioRate) {
                const u32 frames = StrDecode::DecodeXaSector(sector + dataOffset, coding, xa, xaScratch);
                const bool stereo = StrDecode::XaChannels(coding) == 2;
                u32 w = pcmWritten.load(std::memory_order_relaxed);
                for (u32 i = 0; i < frames; ++i, ++w) {
                    const u16 l = static_cast<u16>(xaScratch[stereo ? i * 2 : i]);
                    const u16 r = static_cast<u16>(xaScratch[stereo ? i * 2 + 1 : i]);
                    pcm[w & kPcmMask] = l | (static_cast<u32>(r) << 16);
                }
                pcmWritten.store(w, std::memory_order_release);
            }
        }
        ++audioCursor;
    }
}

void MoviePlayer::Impl::StartAudioIfReady() {
    const bool streamEnded = readerEof.load(std::memory_order_acquire)
        && audioCursor >= written.load(std::memory_order_acquire);
    if (streamEnded) pcmFinal.store(true, std::memory_order_release);
    if (!clockStarted) {
        if (audioSeen && audioRate) {
            if (pcmWritten.load(std::memory_order_relaxed) >= kPcmStartFrames || streamEnded) {
                audioRunning.store(true, std::memory_order_release);
                clockStarted = true;
            }
        }
        else if (streamEnded || audioCursor >= firstFrameSector + 64) {
            // No audio near the start: run on wall time.
            clockStarted = true;
            onWallClock = true;
            wallStartUs = NowMicros();
            wallBase = 0.0f;
        }
        return;
    }
    // Sound over: keep the clock going on wall time for any remaining frames.
    if (!onWallClock && streamEnded
        && pcmRead.load(std::memory_order_acquire) + 1 >= pcmWritten.load(std::memory_order_acquire)) {
        wallBase = Clock();
        wallStartUs = NowMicros();
        onWallClock = true;
        audioDone = true;
    }
}

// Finds the frame starting at or after sector `from`: its index from the
// first frame, the sector after its last chunk and whether all its chunks
// are in the ring. Returns false if there is no video sector between `from`
// and the end of the ring, with `end` at the end of that scan.
bool MoviePlayer::Impl::FindNextFrame(u32 from, u32& frameIndex, u32& end, bool& complete) {
    const u32 available = written.load(std::memory_order_acquire);
    u32 s = from;
    u32 frameNum = 0;
    u32 chunks = 0;
    u32 received = 0;
    bool found = false;
    bool nextFrameSeen = false;
    for (; s < available; ++s) {
        const u8* sector = Sector(s);
        u8 submode = 0, coding = 0;
        bool video = false, audio = false;
        if (!ParseSector(sector, submode, coding, video, audio) || !video) continue;
        const u8* user = sector + dataOffset;
        const u32 num = ReadU32(user + 8);
        if (!found) {
            found = true;
            frameNum = num;
            chunks = ReadU16(user + 6);
            if (!firstFrameSeen) {
                firstFrameSeen = true;
                firstFrameNum = num;
                firstFrameSector = s;
            }
            else if (!intervalKnown && num == firstFrameNum + 1 && s > firstFrameSector) {
                // A 2x CD reads 150 sectors per second.
                frameInterval = static_cast<f32>(s - firstFrameSector) / 150.0f;
                intervalKnown = true;
            }
        }
        else if (num != frameNum) {
            nextFrameSeen = true; // chunks missing from the file: decode what came
            break;
        }
        ++received;
        if (received >= chunks) {
            ++s;
            break;
        }
    }
    end = s;
    if (!found) return false;
    frameIndex = frameNum - firstFrameNum;
    complete = received >= chunks || nextFrameSeen
        || (readerEof.load(std::memory_order_acquire) && s >= available);
    return true;
}

bool MoviePlayer::Impl::DecodeFrameAt(u32 start, u32 end) {
    u32 bytes = 0;
    u32 frameBytes = 0;
    u32 frameWidth = 0;
    u32 frameHeight = 0;
    for (u32 s = start; s < end; ++s) {
        const u8* sector = Sector(s);
        u8 submode = 0, coding = 0;
        bool video = false, audio = false;
        if (!ParseSector(sector, submode, coding, video, audio) || !video) continue;
        const u8* user = sector + dataOffset;
        if (!frameBytes) {
            frameBytes = ReadU32(user + 12);
            frameWidth = ReadU16(user + 16);
            frameHeight = ReadU16(user + 18);
        }
        // Form 2 sectors carry 2328 user bytes, Form 1 2048.
        const u32 userBytes = (sectorSize == SectorSizeData || !(submode & 0x20)) ? 2048u : 2328u;
        u32 copy = userBytes - kVideoChunkHeader;
        if (frameBytes && bytes + copy > frameBytes) copy = frameBytes > bytes ? frameBytes - bytes : 0;
        if (bytes + copy > demuxCapacity) copy = demuxCapacity - bytes;
        std::memcpy(demux + bytes, user + kVideoChunkHeader, copy);
        bytes += copy;
    }
    if (!bytes || frameWidth != width || frameHeight != height) return false;

    auto* native = static_cast<pspTexture*>(texture->GetTexture());
    u16* pixels = native ? native->BeginNative5650Write(static_cast<int>(width), static_cast<int>(height)) : nullptr;
    if (!pixels) return false;
    return StrDecode::DecodeFrame565(demux, bytes, width, height, pixels,
                                     static_cast<u32>(native->GetStorageWidth()));
}

// Shows the newest complete frame that is due; frames passed over on the
// way are skipped without decoding.
void MoviePlayer::Impl::UpdateVideo() {
    if (videoDone) return;
    const f32 now = Clock();
    u32 dueStart = 0;
    u32 dueEnd = 0;
    s32 dueIndex = -1;
    for (;;) {
        u32 index = 0, end = 0;
        bool complete = false;
        if (!FindNextFrame(videoCursor, index, end, complete)) {
            // Audio-only sectors: nothing for the video cursor to wait on.
            videoCursor = end;
            if (readerEof.load(std::memory_order_acquire) && videoCursor >= written.load(std::memory_order_acquire)) {
                videoDone = true;
            }
            break;
        }
        if (!complete) break;
        // Frame 0 shows as soon as the clock starts.
        if (clockStarted ? (static_cast<f32>(index) * frameInterval > now) : (index > 0)) break;
#if defined(PSP_MOVIE_QA)
        if (dueIndex >= 0) ++qaSkipped;
#endif
        dueStart = videoCursor;
        dueEnd = end;
        dueIndex = static_cast<s32>(index);
        videoCursor = end;
        if (!clockStarted) break;
    }
#if defined(PSP_MOVIE_QA)
    const u32 decodeStart = sceKernelGetSystemTimeLow();
#endif
    if (dueIndex >= 0 && dueIndex != shownFrame && DecodeFrameAt(dueStart, dueEnd)) {
        shownFrame = dueIndex;
#if defined(PSP_MOVIE_QA)
        const u32 decodeUs = sceKernelGetSystemTimeLow() - decodeStart;
        ++qaDecoded;
        qaDecodeUsTotal += decodeUs;
        if (decodeUs > qaDecodeUsMax) qaDecodeUsMax = decodeUs;
        const f32 late = now - static_cast<f32>(dueIndex) * frameInterval;
        if (clockStarted && late > qaLateMax) qaLateMax = late;
#endif
    }
}

#if defined(PSP_MOVIE_QA)
void MoviePlayer::Impl::WriteQaReport() const {
    if (qaName.empty()) return;
    std::string upper = qaName;
    for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const f32 wall = static_cast<f32>(NowMicros() - qaOpenUs) / 1000000.0f;
    char report[768];
    std::snprintf(report, sizeof(report),
        "movie=%s\nsize=%lux%lu\ninterval_ms=%.2f\nfinished=%d\n"
        "frames_shown=%ld\nframes_decoded=%lu\nframes_skipped=%lu\n"
        "decode_us_avg=%lu\ndecode_us_max=%lu\nlate_ms_max=%.1f\n"
        "audio_rate=%lu\naudio_underruns=%lu\npcm_min_frames=%lu\nread_us_max=%lu\n"
        "subtitles=%lu\nwall_s=%.2f\nmovie_s=%.2f\nclock_s=%.2f\n",
        qaName.c_str(), static_cast<unsigned long>(width), static_cast<unsigned long>(height),
        static_cast<double>(frameInterval * 1000.0f), finished ? 1 : 0,
        static_cast<long>(shownFrame + 1), static_cast<unsigned long>(qaDecoded),
        static_cast<unsigned long>(qaSkipped),
        static_cast<unsigned long>(qaDecoded ? qaDecodeUsTotal / qaDecoded : 0),
        static_cast<unsigned long>(qaDecodeUsMax), static_cast<double>(qaLateMax * 1000.0f),
        static_cast<unsigned long>(audioRate), static_cast<unsigned long>(qaUnderruns.load()),
        static_cast<unsigned long>(qaPcmMin == 0xFFFFFFFFu ? 0 : qaPcmMin),
        static_cast<unsigned long>(qaReadUsMax.load()), static_cast<unsigned long>(subtitles.size()),
        static_cast<double>(wall), static_cast<double>(static_cast<f32>(shownFrame + 1) * frameInterval),
        static_cast<double>(Clock()));
    p3d::io::WriteTextFile("MOVIE_" + upper + ".TXT", std::string(report));
}
#endif

void MoviePlayer::Impl::Shutdown() {
#if defined(PSP_MOVIE_QA)
    WriteQaReport();
#endif
    audioRunning.store(false, std::memory_order_release);
    pspAudioSetChannelCallback(kMovieAudioChannel, nullptr, nullptr);
    if (readerThread >= 0) {
        stopReader.store(true, std::memory_order_release);
        sceKernelWaitThreadEnd(readerThread, nullptr);
        sceKernelDeleteThread(readerThread);
        readerThread = -1;
    }
    file.Close();
    std::free(ring);
    ring = nullptr;
    delete[] pcm;
    pcm = nullptr;
    delete[] xaScratch;
    xaScratch = nullptr;
    delete[] demux;
    demux = nullptr;
    if (texture) {
        texture->Release();
        texture = nullptr;
    }
}

MoviePlayer::MoviePlayer() {}

MoviePlayer::~MoviePlayer() {
    Close();
}

bool MoviePlayer::Open(const char* path) {
    Close();
    if (!path) return false;
    Impl* m = new Impl();

    if (!m->file.Open(path, p3d::io::FileHandle::Mode::Read)) {
        LOG("[MoviePlayer] Cannot open: %s", path);
        delete m;
        return false;
    }

    u8 head[SectorSizeRaw];
    const size_t headBytes = m->file.Read(head, sizeof(head));
    if (headBytes >= 16 && head[0] == 0x00 && head[1] == 0xFF && head[11] == 0x00) {
        m->sectorSize = SectorSizeRaw;
        m->subHeaderOffset = 16;
        m->dataOffset = 24;
    }
    else if (headBytes >= 12 && IsStrMagic(ReadU32(head + 8))) {
        m->sectorSize = SectorSizeMode2;
        m->subHeaderOffset = 0;
        m->dataOffset = 8;
    }
    else if (headBytes >= 4 && IsStrMagic(ReadU32(head))) {
        m->sectorSize = SectorSizeData;
    }
    else {
        LOG("[MoviePlayer] Unknown sector format: %s", path);
        delete m;
        return false;
    }
    // First video sector: the frame size for the texture.
    const u8* user = head + m->dataOffset;
    if (!IsStrMagic(ReadU32(user)) || !ReadU16(user + 16) || !ReadU16(user + 18)) {
        LOG("[MoviePlayer] No video in the first sector: %s", path);
        delete m;
        return false;
    }
    m->width = ReadU16(user + 16);
    m->height = ReadU16(user + 18);
    m->file.Seek(0, SEEK_SET);

    m->ring = static_cast<u8*>(memalign(64, kRingSectors * m->sectorSize));
    m->pcm = new u32[kPcmFrames];
    m->xaScratch = new s16[StrDecode::kXaSamplesPerSector];
    m->demuxCapacity = kMaxFrameChunks * 2328u;
    m->demux = new u8[m->demuxCapacity];
    m->texture = new tTexture();
    if (!m->ring || !m->texture->Create(static_cast<int>(m->width), static_cast<int>(m->height), 16, 0, nullptr)) {
        LOG("[MoviePlayer] Out of memory for %s", path);
        impl = m;
        Close();
        return false;
    }
    if (m->texture->GetTexture()) m->texture->GetTexture()->SetFilterMode(PDDI_FILTER_BILINEAR);

    // Subtitles in the game language, then language-neutral ones.
    std::string stem(path);
    const size_t slash = stem.find_last_of("/\\");
    if (slash != std::string::npos) stem.erase(0, slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.erase(dot);
    for (auto& c : stem) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string candidates[] = {
        "pc/subtitles/" + stem + "." + LanguageCode() + ".srt",
        "pc/subtitles/" + stem + ".srt",
    };
    for (const std::string& candidate : candidates) {
        if (auto text = p3d::io::ReadTextFile(candidate)) {
            m->subtitles = ParseSrt(*text);
            if (!m->subtitles.empty()) {
                LOG("[MoviePlayer] %u subtitles from %s", static_cast<unsigned>(m->subtitles.size()), candidate.c_str());
                break;
            }
        }
    }

#if defined(PSP_MOVIE_QA)
    m->qaName = stem;
    m->qaOpenUs = NowMicros();
#endif
    impl = m;
    m->readerThread = sceKernelCreateThread("ReChainMovie", ReaderEntry, kReaderPriority, kReaderStackSize,
                                            PSP_THREAD_ATTR_USER, nullptr);
    if (m->readerThread < 0 || sceKernelStartThread(m->readerThread, sizeof(m), &m) < 0) {
        LOG("[MoviePlayer] Reader thread failed for %s", path);
        Close();
        return false;
    }
    pspAudioSetVolume(kMovieAudioChannel, PSP_VOLUME_MAX, PSP_VOLUME_MAX);
    pspAudioSetChannelCallback(kMovieAudioChannel, AudioCallback, m);
    LOG("[MoviePlayer] Opened %s: %ux%u, %u-byte sectors, ring %u KB", path,
        static_cast<unsigned>(m->width), static_cast<unsigned>(m->height), static_cast<unsigned>(m->sectorSize),
        static_cast<unsigned>(kRingSectors * m->sectorSize / 1024));
    return true;
}

void MoviePlayer::Close() {
    if (!impl) return;
    impl->Shutdown();
    delete impl;
    impl = nullptr;
}

bool MoviePlayer::AdvanceFrame() {
    if (!impl || impl->finished) return false;
    Impl& m = *impl;
    m.PumpAudio();
    m.StartAudioIfReady();
    m.UpdateVideo();
#if defined(PSP_MOVIE_QA)
    if (m.audioRunning.load(std::memory_order_relaxed) && !m.pcmFinal.load(std::memory_order_relaxed)) {
        const u32 buffered = m.pcmWritten.load(std::memory_order_relaxed) - m.pcmRead.load(std::memory_order_acquire);
        if (buffered < m.qaPcmMin) m.qaPcmMin = buffered;
    }
#endif
    // Sectors both cursors have passed go back to the reader.
    const u32 done = (m.audioCursor < m.videoCursor) ? m.audioCursor : m.videoCursor;
    if (done > m.released.load(std::memory_order_relaxed)) m.released.store(done, std::memory_order_release);

    const bool audioFinished = !m.audioSeen || m.audioDone;
    if (m.videoDone && audioFinished) {
        // Hold the last frame for its own duration.
        if (!m.clockStarted || m.Clock() >= static_cast<f32>(m.shownFrame + 1) * m.frameInterval) m.finished = true;
    }
    return !m.finished;
}

void MoviePlayer::Render() {
    ScreenDraw::DrawColoredRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 255);
    if (!impl) return;
    Impl& m = *impl;
    if (m.shownFrame >= 0 && m.texture && m.width && m.height) {
        // The whole 4:3 frame, pillarboxed: the credits roll and title
        // cards run close to the frame edges.
        const f32 h = SCREEN_HEIGHT;
        const f32 w = h * 4.0f / 3.0f;
        const f32 x = (SCREEN_WIDTH - w) * 0.5f;
        ScreenDraw::DrawQuad(m.texture, x, 0.0f, w, h);
    }

    if (m.subtitles.empty() || !g_textManager) return;
    const f32 now = m.Clock();
    const Subtitle* active = nullptr;
    for (const Subtitle& cue : m.subtitles) {
        if (now >= cue.start && now <= cue.end) {
            active = &cue;
            break;
        }
    }
    if (!active) return;
    g_textManager->PushState();
    if (g_textManager->SetFontByName("Menu")) {
        // 18 px, the baked-outline strike: one quad per glyph.
        const f32 scale = SCREEN_SCALE_Y(18.0f * DEFAULT_SCREEN_HEIGHT / (32.0f * 272.0f));
        g_textManager->SetScale(scale, scale);
        g_textManager->SetWrapWidth(SCREEN_WIDTH - 140.0f);
        g_textManager->SetLineSpacing(0);
        g_textManager->SetAlignment(TextAlign_Center);
        g_textManager->SetPromptsEnabled(false);
        g_textManager->SetColor(255, 255, 255, 255);
        g_textManager->SetShadow(false);
        g_textManager->SetOutline(true, 1.0f, 0, 0, 0, 255);
        const TextBounds bounds = g_textManager->MeasureString(active->text.c_str());
        g_textManager->PrintString(active->text.c_str(), SCREEN_WIDTH * 0.5f,
                                   SCREEN_HEIGHT - 10.0f - bounds.height);
    }
    g_textManager->PopState();
}

bool MoviePlayer::IsFinished() const { return !impl || impl->finished; }
bool MoviePlayer::IsOpen() const { return impl != nullptr; }
f32 MoviePlayer::GetFrameRate() const { return impl ? 1.0f / impl->frameInterval : 15.0f; }
