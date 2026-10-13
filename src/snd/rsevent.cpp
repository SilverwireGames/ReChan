#include "gen/common.h"
#include "snd/rsevent.h"
#include "snd/sound.h"
#include "snd/soundid.h"
#include "snd/rsdworld.h"
#include "snd/sndmath.h"
#include "snd/ambience.h"
#include "snd/adpcm.h"
#include "radlib/rtask.h"
#include "gen/time.h"
#include "p3d/p3dmath.h"
#include "xclib/xcfile.h"
#include "gen/blockmgr.h"
#include "ai/player.h"
#include "p3d/fileio.h"
#if defined(RC_PLATFORM_PSP)
#include "psp/psp_memory.h"
#include <pspthreadman.h>
#include <atomic>
#endif
#include <cstdlib>
#if MOD_LOADER
#include "extra/modloader.h"
#endif

static constexpr u32 DIALOG_SECTOR_SIZE = 2048;
static constexpr u32 DIALOG_RATE = 11025;

static std::vector<u8> g_dialogFileData;
static std::vector<u8> g_dialogHeader;
static u32 g_dialogHeaderSize = 0;
static u32 g_dialogFileSize = 0;
static u32 g_dialogDataBaseOffset = 0;
static u16 g_lastDialogChoiceOffset = 0;
static s32 g_forcedDialogVariant = -1;
// Menu-preview-only random pick: uniform over all variants, no anti-repeat
// check, never touches g_lastDialogChoiceOffset/used-bits. See LoadDialogClipSample.
static constexpr s32 kIsolatedRandomVariant = -2;

// PC: pending real position for the next RS_PLAY_DIALOG call - see
// SetPendingDialogPlayPosition in rsevent.h.
static LVector g_pendingDialogPlayPosition = {};
static bool g_hasPendingDialogPlayPosition = false;

static bool g_DebugDialogTrace = false;

void SetPendingDialogPlayPosition(const LVector* pos) {
    if (pos) {
        g_pendingDialogPlayPosition = *pos;
        g_hasPendingDialogPlayPosition = true;
    }
    else {
        g_hasPendingDialogPlayPosition = false;
    }
}

static bool ConsumePendingDialogPlayPosition(LVector& outPos) {
    if (!g_hasPendingDialogPlayPosition) {
        return false;
    }
    outPos = g_pendingDialogPlayPosition;
    g_hasPendingDialogPlayPosition = false;
    return true;
}

static void SetDlgStatus(s32 status);
static bool DialogTraceEnabled();

struct DialogClipSelection {
    u32 start = 0;
    u32 end = 0;
    u16 choiceOffset = 0;
    u32 sectors = 0;
    u32 variant = 0;

    u32 ByteSize() const {
        return end - start;
    }
};

static u32 GetDialogPhonographIndexForActiveBank() {
    // PSX LocInfo[loc][6] values at 0x800D6588:
    // normal levels use 46, Chinatown/Sewer bosses use 48, Rooftop boss uses 47.
    if (!g_sound) {
        return 46;
    }

    const s32 bank = g_sound->activeSfxBank;
    if (bank == SND_LOC_CHINA_BOSS || bank == SND_LOC_SEWER_BOSS) {
        return 48;
    }
    if (bank == SND_LOC_ROOF_BOSS) {
        return 47;
    }
    return 46;
}

static u32 GetDialogTransferLimitBytes() {
    if (!g_sound || g_sound->activeSfxBank < 0) {
        return 0;
    }

    const u32 bank = (u32)g_sound->activeSfxBank;
    if (bank >= MAX_WAX_BANKS) {
        return 0;
    }

    const u32 phonographIndex = GetDialogPhonographIndexForActiveBank();
    if (phonographIndex >= MAX_SAMPLES_PER_BANK) {
        return 0;
    }

    // PSX rsdAllocPhonograph returns 2 * descriptor[+8] into gp+1224.
    const u32 halfwords = g_sound->banks[bank].descriptorWord2[phonographIndex];
    return halfwords * 2u;
}

static u8 DialogHeaderU8(u32 offset) {
    if (offset >= g_dialogHeader.size()) {
        return 0;
    }
    return g_dialogHeader[offset];
}

static u16 DialogHeaderU16(u32 offset) {
    if (offset + 1 >= g_dialogHeader.size()) {
        return 0;
    }
    return (u16)((u16)g_dialogHeader[offset] | ((u16)g_dialogHeader[offset + 1] << 8));
}

static void ResetDialogHeaderState() {
    if (g_dialogHeaderSize == 0 || g_dialogFileData.size() < g_dialogHeaderSize) {
        g_dialogHeader.clear();
        return;
    }

    g_dialogHeader.assign(g_dialogFileData.begin(), g_dialogFileData.begin() + g_dialogHeaderSize);
    g_lastDialogChoiceOffset = 0;
}

static bool EnsureDialogDataLoaded() {
    if (!g_dialogFileData.empty() && !g_dialogHeader.empty()) {
        return true;
    }

#if defined(RC_PLATFORM_PSP)
    // RSDIALOG.DLG is a 33 MB container, far larger than the PSP's available
    // user heap once the renderer, fonts and WAX samples are resident. Its
    // header is only a few KB; keep that metadata resident and stream the
    // selected ADPCM clip on demand from the Memory Stick.
    p3d::io::FileHandle file;
    if (!file.Open("SOUND/DIALOG/RSDIALOG.DLG", p3d::io::FileHandle::Mode::Read)
        || file.Size() < 6 || file.Size() > 0xFFFFFFFFll) {
        return false;
    }

    g_dialogFileSize = static_cast<u32>(file.Size());
    u8 sizeBytes[2] = {};
    if (file.Read(sizeBytes, sizeof(sizeBytes)) != sizeof(sizeBytes)) {
        g_dialogFileSize = 0;
        return false;
    }

    g_dialogHeaderSize = (u32)((u16)sizeBytes[0] | ((u16)sizeBytes[1] << 8));
    if (g_dialogHeaderSize == 0 || g_dialogHeaderSize > g_dialogFileSize) {
        g_dialogHeaderSize = 0;
        g_dialogFileSize = 0;
        return false;
    }

    g_dialogFileData.resize(g_dialogHeaderSize);
    if (!file.Seek(0, SEEK_SET)
        || file.Read(g_dialogFileData.data(), g_dialogFileData.size()) != g_dialogFileData.size()) {
        g_dialogFileData.clear();
        g_dialogHeaderSize = 0;
        g_dialogFileSize = 0;
        return false;
    }

    g_dialogDataBaseOffset = (g_dialogHeaderSize + (DIALOG_SECTOR_SIZE - 1))
        & ~(DIALOG_SECTOR_SIZE - 1);
    if (g_dialogDataBaseOffset >= g_dialogFileSize) {
        g_dialogFileData.clear();
        g_dialogHeaderSize = 0;
        g_dialogFileSize = 0;
        g_dialogDataBaseOffset = 0;
        return false;
    }

    ResetDialogHeaderState();
    return !g_dialogHeader.empty();
#else
    u8* raw = nullptr;
    u32 size = 0;
    if (!xcReadFileLow("SOUND/DIALOG/RSDIALOG.DLG", &raw, &size) || !raw || size < 6) {
        if (raw) {
            delete[] raw;
        }
        return false;
    }

    g_dialogFileData.assign(raw, raw + size);
    delete[] raw;
    g_dialogFileSize = size;

    g_dialogHeaderSize = (u32)((u16)g_dialogFileData[0] | ((u16)g_dialogFileData[1] << 8));
    if (g_dialogHeaderSize == 0 || g_dialogHeaderSize > g_dialogFileData.size()) {
        g_dialogFileData.clear();
        g_dialogHeader.clear();
        g_dialogHeaderSize = 0;
        g_dialogFileSize = 0;
        return false;
    }

    g_dialogDataBaseOffset = (g_dialogHeaderSize + (DIALOG_SECTOR_SIZE - 1)) & ~(DIALOG_SECTOR_SIZE - 1);
    if (g_dialogDataBaseOffset >= g_dialogFileData.size()) {
        g_dialogFileData.clear();
        g_dialogHeader.clear();
        g_dialogHeaderSize = 0;
        g_dialogFileSize = 0;
        g_dialogDataBaseOffset = 0;
        return false;
    }

    ResetDialogHeaderState();
    return !g_dialogHeader.empty();
#endif
}

static bool SelectDialogClipOffset(s32 character, s32 dialogID, u32* outStart, u32* outEnd,
                                   u16* outChoiceOffset, u32* outSectors, u32* outVariant) {
    if (!outStart || !outEnd || !outChoiceOffset || !outSectors || !outVariant) {
        return false;
    }

    if (!EnsureDialogDataLoaded()) {
        return false;
    }

    const u32 charTableOffset = 4;
    const u32 charEntryOffset = charTableOffset + ((u32)(u16)character * 2);
    if (charEntryOffset + 1 >= g_dialogHeaderSize) {
        return false;
    }

    const u16 charInfoOffset = DialogHeaderU16(charEntryOffset);
    if (charInfoOffset >= g_dialogHeaderSize) {
        return false;
    }

    const u8 dialogCount = DialogHeaderU8(charInfoOffset);
    if (dialogCount == 0) {
        SetDlgStatus(8);
        return false;
    }

    const u32 dialogListOffset = (u32)charInfoOffset + 1;
    const u32 streamTableOffset = dialogListOffset + (u32)dialogCount;
    const u32 clipInfoTableOffset = streamTableOffset + ((u32)dialogCount * 2);
    if (clipInfoTableOffset + ((u32)dialogCount * 2) > g_dialogHeaderSize) {
        return false;
    }

    s32 dialogIndex = -1;
    for (u32 i = 0; i < (u32)dialogCount; ++i) {
        if ((s32)DialogHeaderU8(dialogListOffset + i) == dialogID) {
            dialogIndex = (s32)i;
            break;
        }
    }
    if (dialogIndex < 0) {
        SetDlgStatus(8);
        return false;
    }

    const u32 streamSectors = (u32)DialogHeaderU16(streamTableOffset + ((u32)dialogIndex * 2));
    const u32 streamStartBase = g_dialogDataBaseOffset + streamSectors * DIALOG_SECTOR_SIZE;

    const u16 clipInfoOffset = DialogHeaderU16(clipInfoTableOffset + ((u32)dialogIndex * 2));
    if (clipInfoOffset >= g_dialogHeaderSize) {
        return false;
    }

    const u8 clipCount = DialogHeaderU8(clipInfoOffset);
    if (clipCount == 0) {
        SetDlgStatus(8);
        return false;
    }

    const u32 clipSizeTableOffset = (u32)clipInfoOffset + 1;
    if (clipSizeTableOffset + clipCount > g_dialogHeaderSize) {
        return false;
    }

    const u32 transferLimitBytes = GetDialogTransferLimitBytes();

    while (true) {
        u32 selected = 0;
        if (g_forcedDialogVariant >= 0) {
            selected = static_cast<u32>(g_forcedDialogVariant);
            if (selected >= clipCount) return false;
        }
        else if (g_forcedDialogVariant == kIsolatedRandomVariant) {
            selected = (u32)rmRangedRandom((s32)clipCount);
        }
        else {
            u8 available[256] = {};
            u32 availableCount = 0;
            for (u32 i = 0; i < (u32)clipCount; ++i) {
                const u8 flags = DialogHeaderU8(clipSizeTableOffset + i);
                if ((flags & 0xC0) == 0) {
                    available[availableCount++] = (u8)i;
                }
            }

            if (availableCount == 0) {
                for (u32 i = 0; i < (u32)clipCount; ++i) {
                    const u32 off = clipSizeTableOffset + i;
                    const u8 flags = DialogHeaderU8(off);
                    if ((flags & 0x80) != 0 && (flags & 0x40) == 0) {
                        g_dialogHeader[off] = (u8)(flags & 0x3F);
                    }
                }

                for (u32 i = 0; i < (u32)clipCount; ++i) {
                    const u8 flags = DialogHeaderU8(clipSizeTableOffset + i);
                    if ((flags & 0xC0) == 0) {
                        available[availableCount++] = (u8)i;
                    }
                }
            }

            if (availableCount == 0) {
                SetDlgStatus(9);
                return false;
            }

            const u32 selectedSlot = (u32)rmRangedRandom((s32)availableCount);
            selected = (u32)available[selectedSlot];
        }
        const u16 choiceOffset = (u16)(clipSizeTableOffset + selected);

        // PSX SelectDialog rejects immediate repeat selection instead of rerolling.
        // Only gameplay's own shared random pick (-1) participates in this -
        // the isolated menu-preview pick has no notion of "last played".
        if (g_forcedDialogVariant == -1 && choiceOffset == g_lastDialogChoiceOffset) {
            if (DialogTraceEnabled()) {
                LOG("[DialogTrace] SELECT-REPEAT bail char=%d dialog=%d choiceOffset=%u lastChoice=%u clipCount=%u",
                    character, dialogID, choiceOffset, g_lastDialogChoiceOffset, (u32)clipCount);
            }
            SetDlgStatus(9);
            return false;
        }

        u32 streamStart = streamStartBase;
        for (u32 i = 0; i < selected; ++i) {
            const u32 clipSectors = (u32)(DialogHeaderU8(clipSizeTableOffset + i) & 0x3F);
            streamStart += clipSectors * DIALOG_SECTOR_SIZE;
        }

        const u32 selectedSectors = (u32)(DialogHeaderU8(choiceOffset) & 0x3F);
        const u32 clipSize = selectedSectors * DIALOG_SECTOR_SIZE;
        if (clipSize == 0) {
            return false;
        }

        if (transferLimitBytes < clipSize) {
            SetDlgStatus(10);
            if ((u32)choiceOffset < g_dialogHeader.size()) {
                g_dialogHeader[choiceOffset] = (u8)(DialogHeaderU8(choiceOffset) | 0x40);
            }
            continue;
        }

        const u32 clipEnd = streamStart + clipSize;
        if (streamStart >= g_dialogFileSize || clipEnd > g_dialogFileSize || clipEnd < streamStart) {
            return false;
        }

        *outStart = streamStart;
        *outEnd = clipEnd;
        *outChoiceOffset = choiceOffset;
        *outSectors = selectedSectors;
        *outVariant = selected;
        return true;
    }
}

static bool SelectDialogClip(s32 character, s32 dialogID, DialogClipSelection* outSelection) {
    if (!outSelection) {
        return false;
    }

    DialogClipSelection selection;
    if (!SelectDialogClipOffset(character, dialogID, &selection.start, &selection.end,
                                &selection.choiceOffset, &selection.sectors, &selection.variant)) {
        return false;
    }

    *outSelection = selection;
    return true;
}

#if defined(RC_PLATFORM_PSP)
// RSDIALOG.DLG stays open between clips: reopening the 33 MB container (open,
// then a seek to its end for the size) cost up to ~130 ms per voice in combat.
static p3d::io::FileHandle g_dialogClipFile;

// Recently played clips' ADPCM, large heap only: combat grunts repeat often.
struct DialogClipCacheEntry {
    u32 start = 0;
    u32 lastUse = 0;
    std::vector<u8> data;
};
static constexpr u32 kDialogClipCacheBudget = 3u * 1024u * 1024u;
static std::vector<DialogClipCacheEntry> g_dialogClipCache;
static u32 g_dialogClipCacheBytes = 0;
static u32 g_dialogClipCacheClock = 0;

static const std::vector<u8>* FindCachedDialogClip(u32 start, u32 size) {
    for (DialogClipCacheEntry& entry : g_dialogClipCache) {
        if (entry.start == start && entry.data.size() == size) {
            entry.lastUse = ++g_dialogClipCacheClock;
            return &entry.data;
        }
    }
    return nullptr;
}

static void CacheDialogClip(u32 start, const std::vector<u8>& data) {
    if (!PspHasLargeHeap() || data.size() > kDialogClipCacheBudget / 4u) return;
    while (!g_dialogClipCache.empty()
        && g_dialogClipCacheBytes + data.size() > kDialogClipCacheBudget) {
        auto oldest = g_dialogClipCache.begin();
        for (auto it = g_dialogClipCache.begin(); it != g_dialogClipCache.end(); ++it) {
            if (it->lastUse < oldest->lastUse) oldest = it;
        }
        g_dialogClipCacheBytes -= static_cast<u32>(oldest->data.size());
        g_dialogClipCache.erase(oldest);
    }
    DialogClipCacheEntry entry;
    entry.start = start;
    entry.lastUse = ++g_dialogClipCacheClock;
    entry.data = data;
    g_dialogClipCacheBytes += static_cast<u32>(entry.data.size());
    g_dialogClipCache.push_back(std::move(entry));
}

static bool ReadDialogClip(u32 start, std::vector<u8>& out) {
    // One retry with a fresh handle (e.g. after the Memory Stick was reset).
    for (u32 attempt = 0; attempt < 2u; ++attempt) {
        if (!g_dialogClipFile.IsOpen()
            && !g_dialogClipFile.Open("SOUND/DIALOG/RSDIALOG.DLG", p3d::io::FileHandle::Mode::Read)) {
            return false;
        }
        if (g_dialogClipFile.Seek(start, SEEK_SET)
            && g_dialogClipFile.Read(out.data(), out.size()) == out.size()) {
            return true;
        }
        g_dialogClipFile.Close();
    }
    return false;
}
#endif


static AudioSample LoadDialogSampleFromSelection(s32 character, s32 dialogID,
                                                 const DialogClipSelection& selection) {
    if (selection.start >= g_dialogFileSize
        || selection.end > g_dialogFileSize
        || selection.start >= selection.end) {
        return AUDIO_SAMPLE_INVALID;
    }

#if MOD_LOADER
    char overrideName[64];
    std::snprintf(overrideName, sizeof(overrideName), "c%02d_d%03d_v%02u",
                  character, dialogID, selection.variant);
    std::vector<s16> overridePcm;
    u32 overrideRate = 0, overrideChannels = 0;
    if (ModLoader::Instance().GetSoundOverridePCM(
            "dialog", overrideName, overridePcm, overrideRate, overrideChannels)
        && !overridePcm.empty() && overrideRate > 0 && overrideChannels > 0) {
        AudioSample overrideSample = AudioEngine::LoadSample(
            overridePcm.data(), static_cast<u32>(overridePcm.size()), overrideRate, overrideChannels);
        if (overrideSample != AUDIO_SAMPLE_INVALID) {
            LOG("[Dialog] Override: dialog/%s", overrideName);
            return overrideSample;
        }
    }
#endif

    const u32 clipSize = selection.ByteSize();
#if defined(RC_PLATFORM_PSP)
    const std::vector<u8>* cachedClip = FindCachedDialogClip(selection.start, clipSize);
    std::vector<u8> clipData;
    if (!cachedClip) {
        clipData.resize(clipSize);
        if (!ReadDialogClip(selection.start, clipData)) {
            return AUDIO_SAMPLE_INVALID;
        }
        CacheDialogClip(selection.start, clipData);
    }
    std::vector<s16> pcm = SpuAdpcm::Decode(
        cachedClip ? cachedClip->data() : clipData.data(), clipSize, true);
#else
    const u8* clipData = &g_dialogFileData[selection.start];
    std::vector<s16> pcm = SpuAdpcm::Decode(clipData, clipSize, true);
#endif
    if (DialogTraceEnabled()) {
        LOG("[DialogTrace] decode char=%d dialog=%d variant=%u sectors=%u adpcmBytes=%u "
            "start=0x%X pcmFrames=%u dur=%.3fs",
            character, dialogID, selection.variant, selection.sectors, clipSize,
            selection.start, (u32)pcm.size(),
            (f32)pcm.size() / (f32)DIALOG_RATE);
    }
    if (pcm.empty()) {
        return AUDIO_SAMPLE_INVALID;
    }

    AudioSample sample = AudioEngine::LoadSample(pcm.data(), (u32)pcm.size(), DIALOG_RATE, 1);
    if (sample == AUDIO_SAMPLE_INVALID) {
        return AUDIO_SAMPLE_INVALID;
    }

    return sample;
}

static AudioSample LoadDialogSample(s32 character, s32 dialogID, u16* outChoiceOffset, u32* outSectors) {
    if (!outChoiceOffset || !outSectors) {
        return AUDIO_SAMPLE_INVALID;
    }

    DialogClipSelection selection;
    if (!SelectDialogClip(character, dialogID, &selection)) {
        return AUDIO_SAMPLE_INVALID;
    }

    AudioSample sample = LoadDialogSampleFromSelection(character, dialogID, selection);
    if (sample == AUDIO_SAMPLE_INVALID) {
        return AUDIO_SAMPLE_INVALID;
    }

    *outChoiceOffset = selection.choiceOffset;
    *outSectors = selection.sectors;
    return sample;
}

struct DialogEntry {
    s32 handle;
    s32 character;
    s32 dialogId;
    s32 priority;
    AudioSample sample;
    AudioVoice voice;
    bool valid;
#if defined(RC_PLATFORM_PSP)
    // Non-zero while the dialog worker is still reading this entry's clip.
    u32 asyncSerial;
#endif
};

static constexpr s32 DIALOG_ENTRY_COUNT = 64;
static DialogEntry g_dialogEntries[DIALOG_ENTRY_COUNT] = {};

#if defined(RC_PLATFORM_PSP)
// Voice clips not in the cache are read and decoded by a low-priority worker
// thread, as the PSX streamed them from CD: a seek inside the 33 MB container
// measured 50-100 ms on the Memory Stick, a whole stall of the game thread.
// The dialog state machine already waits for a CD completion before it plays a
// loaded clip (CompletePendingDialogLoad), so that completion simply waits for
// the worker. The worker never allocates: the game thread opens the file and
// sizes both buffers before submitting a job.
struct DialogAsyncJob {
    u32 serial = 0;
    s32 entryHandle = 0;
    u32 start = 0;
    u32 size = 0;
    std::vector<u8> adpcm;
    std::vector<s16> pcm;
    u32 pcmSamples = 0;
    bool ok = false;
};
static constexpr u32 kDialogJobIdle = 0;
static constexpr u32 kDialogJobRunning = 1;
static constexpr u32 kDialogJobDone = 2;
// Above the game thread (0x20), which with triple buffering rarely blocks, and
// below the music worker (0x1C). The decode yields between slices so a clip
// never takes the CPU from a frame in one piece.
static constexpr int kDialogWorkerPriority = 0x1E;
static constexpr u32 kDialogDecodeSliceBlocks = 128;  // 3584 samples
static constexpr SceUInt kDialogDecodeYieldUs = 500;
static DialogAsyncJob g_dialogJob;
static std::atomic<u32> g_dialogJobState{kDialogJobIdle};
static u32 g_dialogJobSerial = 0;
static SceUID g_dialogJobSema = -1;
static SceUID g_dialogWorkerThread = -1;
static p3d::io::FileHandle g_dialogWorkerFile;

static int DialogWorkerEntry(SceSize, void*) {
    while (sceKernelWaitSema(g_dialogJobSema, 1, nullptr) >= 0) {
        if (g_dialogJobState.load(std::memory_order_acquire) != kDialogJobRunning) continue;
        DialogAsyncJob& job = g_dialogJob;
        job.ok = false;
        job.pcmSamples = 0;
        if (g_dialogWorkerFile.Seek(job.start, SEEK_SET)
            && g_dialogWorkerFile.Read(job.adpcm.data(), job.size) == job.size) {
            // Same decode as SpuAdpcm::Decode(..., stopAtEnd = true).
            s32 prev0 = 0;
            s32 prev1 = 0;
            const u32 blocks = job.size / 16u;
            for (u32 block = 0; block < blocks; ++block) {
                const bool ended = SpuAdpcm::DecodeBlock(job.adpcm.data() + block * 16u,
                    job.pcm.data() + job.pcmSamples, prev0, prev1);
                job.pcmSamples += 28u;
                if (ended) break;
                if ((block + 1u) % kDialogDecodeSliceBlocks == 0u) {
                    sceKernelDelayThread(kDialogDecodeYieldUs);
                }
            }
            job.ok = job.pcmSamples != 0;
        }
        g_dialogJobState.store(kDialogJobDone, std::memory_order_release);
    }
    return sceKernelExitThread(0);
}

static bool EnsureDialogWorker() {
    if (g_dialogWorkerThread >= 0) return true;
    g_dialogJobSema = sceKernelCreateSema("ReChainDialogJob", 0, 0, 1, nullptr);
    if (g_dialogJobSema < 0) return false;
    g_dialogWorkerThread = sceKernelCreateThread("ReChainDialog", DialogWorkerEntry,
        kDialogWorkerPriority, 0x4000, PSP_THREAD_ATTR_USER, nullptr);
    if (g_dialogWorkerThread < 0 || sceKernelStartThread(g_dialogWorkerThread, 0, nullptr) < 0) {
        if (g_dialogWorkerThread >= 0) sceKernelDeleteThread(g_dialogWorkerThread);
        sceKernelDeleteSema(g_dialogJobSema);
        g_dialogWorkerThread = -1;
        g_dialogJobSema = -1;
        return false;
    }
    return true;
}

static DialogEntry* FindDialogEntry(s32 handle);

// Attaches a finished job's clip to its entry (game thread). Returns false
// while the worker is still reading; wait = true blocks until it is done.
static bool FinishDialogAsyncLoad(bool wait) {
    u32 state = g_dialogJobState.load(std::memory_order_acquire);
    if (state == kDialogJobIdle) return true;
    while (state == kDialogJobRunning) {
        if (!wait) return false;
        sceKernelDelayThread(1000);
        state = g_dialogJobState.load(std::memory_order_acquire);
    }

    DialogAsyncJob& job = g_dialogJob;
    DialogEntry* entry = FindDialogEntry(job.entryHandle);
    const bool entryWaiting = entry && entry->valid && entry->asyncSerial == job.serial;
    if (job.ok) {
        CacheDialogClip(job.start, job.adpcm);
    }
    else {
        g_dialogWorkerFile.Close();  // reopened by the next submission
    }
    if (entryWaiting) {
        entry->asyncSerial = 0;
        if (job.ok) {
            entry->sample = AudioEngine::LoadSample(job.pcm.data(), job.pcmSamples, DIALOG_RATE, 1);
        }
    }
    g_dialogJobState.store(kDialogJobIdle, std::memory_order_release);
    return true;
}

// Starts reading entry's clip in the background. False when the clip should
// load synchronously instead (cached, or the worker/file is unavailable).
static bool SubmitDialogAsyncLoad(DialogEntry* entry, const DialogClipSelection& selection) {
    const u32 clipSize = selection.ByteSize();
    if (!entry || clipSize < 16u || FindCachedDialogClip(selection.start, clipSize)) return false;
    FinishDialogAsyncLoad(true);  // one job at a time
    if (!EnsureDialogWorker()) return false;
    if (!g_dialogWorkerFile.IsOpen()
        && !g_dialogWorkerFile.Open("SOUND/DIALOG/RSDIALOG.DLG", p3d::io::FileHandle::Mode::Read)) {
        return false;
    }

    DialogAsyncJob& job = g_dialogJob;
    if (++g_dialogJobSerial == 0) ++g_dialogJobSerial;
    job.serial = g_dialogJobSerial;
    job.entryHandle = entry->handle;
    job.start = selection.start;
    job.size = clipSize;
    job.adpcm.resize(clipSize);
    job.pcm.resize(static_cast<std::size_t>(clipSize / 16u) * 28u);
    job.pcmSamples = 0;
    job.ok = false;
    entry->asyncSerial = job.serial;
    g_dialogJobState.store(kDialogJobRunning, std::memory_order_release);
    sceKernelSignalSema(g_dialogJobSema, 1);
    return true;
}
#endif
static s32 g_nextDialogHandle = 1;
// PSX: dword_800D8808 - default dialog handle used by jcsIsPlaying__Fv.
static s32 g_primaryDialogHandle = 0;
// PSX: dword_800D880C - secondary dialog handle used by handle-role checks.
static s32 g_secondaryDialogHandle = 0;
// PSX: g_Dlg[0] and dword_800D8824 - primary/secondary dialog priorities.
static s32 g_primaryDialogPriority = -1;
static s32 g_secondaryDialogPriority = -1;
// PSX: gp+1212 - JCSDLG state machine status used by handle validation/playability.
static s32 g_dialogStatus = 0;
// PSX: gp+1208 - dialog system enabled by jcsStartDialog / disabled by jcsStopDialog.
static s32 g_dialogSystemStarted = 0;
// PSX: gp+1220 - dialogue voice captured/locked by jcsSetLevelDialog.
static AudioVoice g_dialogReservedVoice = AUDIO_VOICE_INVALID;
// Host bookkeeping for the PSX CD completion callback state transition.
static s32 g_dialogLoadPending = 0;
static s32 g_dialogLoadPendingState = -1;
static s32 g_dialogLoadCompletionState = -1;
// A transfer owns its completion. Deferred states are allowed to relabel the
// pending transfer (as on PSX), but unrelated task ticks must not dispatch the
// CD callback and consume a dialog state transition.
static s32 g_dialogLoadDueFrame = -1;
static u32 g_dialogLoadSerial = 0;
// A real CD completion is asynchronous. One frame is enough to retain the
// load window without allowing a normal play request to complete it early.
static constexpr s32 kDialogLoadLatencyFrames = 1;
// PSX: gp+3520/gp+3524 - deferred dialog character and id.
static s32 g_deferredDialogCharacter = 0;
static s32 g_deferredDialogId = 0;
// PSX: g_Dlg queue entries (8 words per queue, primary + secondary).
struct DialogQueueInfo {
    s32 priority = 0;      // +0
    s32 handle = 0;        // +4
    s32 sectors = 0;       // +8
    s32 timeoutFrame = 0;  // +12
    s32 playPosition = 0;  // +16 (tagLVector pointer/value in PSX ABI)
    u16 clipEntry = 0;     // +20
    u16 pad = 0;           // +22
    s32 flushFrame = 0;    // +24
    s32 dialogId = 112;    // +28
};
static_assert(sizeof(DialogQueueInfo) == 32, "DialogQueueInfo must match PSX g_Dlg row size");
static DialogQueueInfo g_dialogQueues[2] = {};
// PC: real (non-truncated) position paired 1:1 with g_dialogQueues[], kept
// outside DialogQueueInfo so that struct keeps mirroring the 32-byte PSX
// g_Dlg row exactly. playPosition above stays a raw s32 for ABI fidelity;
// this is what actually feeds spatial audio at play time.
struct DialogQueuePosition {
    LVector pos = {};
    bool hasPos = false;
};
static DialogQueuePosition g_dialogQueuePos[2] = {};
// PSX: gp+1236 - SetDlgStatus output value.
static s32 g_dialogResultStatus = 0;
static bool g_dialogStatusUpdateInProgress = false;
// PSX gp+1188: task returned by jcsInitializeDialog__Fv.
static RTASK* g_dialogTask = nullptr;

// PSX gp+740: the rsdAmbiance object. PSX services it via an RTASK on
// rMainTaskList; on PC that scheduling is unreliable because MenuFade resets
// rFrameCount60 (game.cpp) without re-basing the task list, stalling the task
// for a long window after every level transition. Instead we tick it directly
// from the frame loops (jcsUpdateAmbience, called from main.cpp + the menu-fade
// sub-loop) so its fades/crossfades advance every frame in every game state.
static Ambiance g_ambiance;

// Called once per frame from the main loop and the blocking menu-fade loop.
void jcsUpdateAmbience() {
    g_ambiance.Update();
}

static bool DialogTraceEnabled() {
    return g_DebugDialogTrace;
}

enum DialogStatus : s32 {
    DialogStatus_Idle = 0,
    DialogStatus_LoadRequestedPrimary = 1,
    DialogStatus_LoadDeferredPrimary = 2,
    DialogStatus_KillRequestedPrimary = 3,
    DialogStatus_LoadDeferredSecondary = 4,
    DialogStatus_LoadedPrimary = 5,
    DialogStatus_LoadedPrimaryWithDeferred = 6,
    DialogStatus_LoadedPrimaryWithDeferredFromState2 = 7,
    DialogStatus_LoadedPrimaryWithDeferredFromState3 = 8,
    DialogStatus_LoadedPrimaryAndSecondary = 9,
    DialogStatus_PlayingPrimary = 10,
    DialogStatus_PlayingPrimaryWithSecondary = 11,
    DialogStatus_PlayingPrimaryState12 = 12,
    DialogStatus_PlayingPrimaryState13 = 13,
    DialogStatus_PlayingPrimaryState14 = 14,
};

// 30 Hz count from the wall clock; g_time ticks per rendered frame in menus.
static s32 GetDialogFrameCounter() {
    // +1: a stored frame of 0 means "unset" in the queue bookkeeping.
    return (s32)(Time::GetTimeInSeconds() * 30.0) + 1;
}

s32 jcsValidateHandle(s32 handle);
static DialogEntry* FindDialogEntry(s32 handle);
static void StopDialogEntryVoice(DialogEntry* entry);
static void ReleaseDialogEntry(DialogEntry* entry);
static void ClearDialogPendingLoadState();
static void BeginDialogPendingLoad(s32 pendingState);
static bool CompletePendingDialogLoad(bool force);
static void CDDoneCallback();
static void LoadDialogToAudioMemory();
static void UpdateDialogStatusFromSlots();
static void ProcessDialogTask();
static s32 DialogTask(RTASK* task);
static RTASK* jcsInitializeDialog();
static void jcsTerminateDialog();
static s32 IsPrimaryHandle(s32 handle);
static s32 PlayDialogHandle(DialogEntry* entry, const LVector* posPtr);
static s32 PlayLoadedState(s32 playPosition, const LVector* posPtr, s32 nextState);
static s32 PlayDialogRequest(s32 handleParam, s32 playPosition, const LVector* posPtr, u32 timeout);
static s32 RequestLoadDialog(s32 existingHandle, s32 character, s32 dialogId,
                             s32 priority, s32 queue, s32 state);
static s32 jcsLoadDialog(s32 character, s32 dialogId, s32 priority);
static void KillAllDialogHandles();
static s32 jcsStopDialog();

// PSX: SetDlgStatus__F9DlgStatus
static void SetDlgStatus(s32 status) {
    g_dialogResultStatus = status;
    if (DialogTraceEnabled()) {
        LOG("[DialogTrace] result=%d state=%d primary=%d secondary=%d pending=%d",
            status, g_dialogStatus, g_primaryDialogHandle, g_secondaryDialogHandle, g_dialogLoadPending);
    }
}

// PSX: ResetDialogInfo__F5Queue5State queue field reset (without state write).
static void ResetDialogQueueInfo(s32 queue) {
    if (queue < 0 || queue >= 2) {
        return;
    }

    g_dialogQueues[queue] = {};
    g_dialogQueues[queue].dialogId = 112;
    g_dialogQueuePos[queue] = {};
}

// PSX: UpdateTimeOut__FUlPC10tagLVector5Queue
static s32 UpdateDialogTimeout(u32 timeout, s32 playPosition, const LVector* posPtr, s32 queue) {
    if (timeout == 0u) {
        SetDlgStatus(7);
        return 0;
    }

    if (queue < 0 || queue >= 2) {
        return 0;
    }

    DialogQueueInfo& q = g_dialogQueues[queue];
    const s32 frameCounter = GetDialogFrameCounter();
    q.playPosition = playPosition;
    q.timeoutFrame = frameCounter + static_cast<s32>(timeout);
    if (posPtr) {
        g_dialogQueuePos[queue].pos = *posPtr;
        g_dialogQueuePos[queue].hasPos = true;
    }
    else {
        g_dialogQueuePos[queue].hasPos = false;
    }
    return 1;
}

// PSX: PauseTimeOut__F5Queue
static void PauseDialogTimeout(s32 queue) {
    if (queue < 0 || queue >= 2) {
        return;
    }

    DialogQueueInfo& q = g_dialogQueues[queue];
    if (q.timeoutFrame == 0 || !jcsValidateHandle(q.handle)) {
        return;
    }

    const s32 frameCounter = GetDialogFrameCounter();
    if (frameCounter < q.timeoutFrame) {
        q.timeoutFrame = q.timeoutFrame - frameCounter;
    }
    else {
        q.timeoutFrame = 1;
    }
}

// PSX: ResumeTimeOut__F5Queue
static void ResumeDialogTimeout(s32 queue) {
    if (queue < 0 || queue >= 2) {
        return;
    }

    DialogQueueInfo& q = g_dialogQueues[queue];
    if (q.timeoutFrame == 0 || !jcsValidateHandle(q.handle)) {
        return;
    }

    q.timeoutFrame = q.timeoutFrame + GetDialogFrameCounter();
}

static s32 ResolveDialogHandleParam(s32 handle) {
    if (handle != 0) {
        return handle;
    }
    return g_primaryDialogHandle;
}

static void UpdateDialogState(s32 state) {
    if (DialogTraceEnabled() && g_dialogStatus != state) {
        LOG("[DialogTrace] state %d -> %d primary=%d secondary=%d pending=%d",
            g_dialogStatus, state, g_primaryDialogHandle, g_secondaryDialogHandle, g_dialogLoadPending);
    }
    g_dialogStatus = state;
}

static void ResetDialogInfoAndState(s32 queue, s32 state) {
    if (queue == 0) {
        DialogEntry* primaryEntry = FindDialogEntry(g_primaryDialogHandle);
        ReleaseDialogEntry(primaryEntry);
        g_primaryDialogHandle = 0;
        g_primaryDialogPriority = -1;
    }
    else if (queue == 1) {
        DialogEntry* secondaryEntry = FindDialogEntry(g_secondaryDialogHandle);
        ReleaseDialogEntry(secondaryEntry);
        g_secondaryDialogHandle = 0;
        g_secondaryDialogPriority = -1;
    }

    ResetDialogQueueInfo(queue);
    UpdateDialogState(state);
    ClearDialogPendingLoadState();
}

static void UpgradeDialogInfoAndState(s32 state) {
    // PSX UpgradeDlgInfo copies queue1 -> queue0 and resets queue1.
    DialogEntry* primaryEntry = FindDialogEntry(g_primaryDialogHandle);
    DialogEntry* secondaryEntry = FindDialogEntry(g_secondaryDialogHandle);

    if (primaryEntry && (!secondaryEntry || primaryEntry->handle != secondaryEntry->handle)) {
        ReleaseDialogEntry(primaryEntry);
    }

    if (secondaryEntry) {
        g_dialogQueues[0] = g_dialogQueues[1];
        g_primaryDialogHandle = g_secondaryDialogHandle;
        g_primaryDialogPriority = g_secondaryDialogPriority;
    }
    else {
        g_primaryDialogHandle = 0;
        g_primaryDialogPriority = -1;
        ResetDialogQueueInfo(0);
    }

    g_secondaryDialogHandle = 0;
    g_secondaryDialogPriority = -1;
    ResetDialogQueueInfo(1);
    UpdateDialogState(state);
    // PC: the promoted queue-0 entry is already fully decoded
    BeginDialogPendingLoad(state);
}

static void UpgradeToPrimaryState() {
    UpgradeDialogInfoAndState(DialogStatus_LoadedPrimary);
    LoadDialogToAudioMemory();
}

static s32 KillDialogByHandleInternal(s32 handleParam) {
    const s32 handle = handleParam;
    if (!jcsValidateHandle(handle)) {
        return 0;
    }

    if (DialogTraceEnabled()) {
        LOG("[DialogTrace] KILL handle=%d state=%d primary=%d secondary=%d",
            handle, g_dialogStatus, g_primaryDialogHandle, g_secondaryDialogHandle);
    }

    switch (g_dialogStatus) {
        case DialogStatus_Idle:
        case DialogStatus_KillRequestedPrimary:
            return 0;

        case DialogStatus_LoadRequestedPrimary:
        case DialogStatus_LoadDeferredPrimary:
            if (!IsPrimaryHandle(handle)) {
                return 0;
            }
            UpdateDialogState(DialogStatus_KillRequestedPrimary);
            return 1;

        case DialogStatus_LoadDeferredSecondary:
            if (IsPrimaryHandle(handle)) {
                return 0;
            }
            ResetDialogInfoAndState(1, DialogStatus_KillRequestedPrimary);
            return 1;

        case DialogStatus_LoadedPrimary:
            if (!IsPrimaryHandle(handle)) {
                return 0;
            }
            ResetDialogInfoAndState(0, DialogStatus_Idle);
            return 1;

        case DialogStatus_LoadedPrimaryWithDeferred:
            if (IsPrimaryHandle(handle)) {
                UpgradeDialogInfoAndState(DialogStatus_LoadRequestedPrimary);
            }
            else {
                UpdateDialogState(DialogStatus_LoadedPrimaryWithDeferredFromState3);
            }
            return 1;

        case DialogStatus_LoadedPrimaryWithDeferredFromState2:
            if (IsPrimaryHandle(handle)) {
                UpgradeDialogInfoAndState(DialogStatus_LoadDeferredPrimary);
            }
            else {
                UpdateDialogState(DialogStatus_LoadedPrimaryWithDeferredFromState3);
            }
            return 1;

        case DialogStatus_LoadedPrimaryWithDeferredFromState3:
            if (!IsPrimaryHandle(handle)) {
                return 0;
            }
            UpgradeDialogInfoAndState(DialogStatus_KillRequestedPrimary);
            return 1;

        case DialogStatus_LoadedPrimaryAndSecondary:
            if (IsPrimaryHandle(handle)) {
                UpgradeToPrimaryState();
            }
            else {
                ResetDialogInfoAndState(1, DialogStatus_LoadedPrimary);
            }
            return 1;

        case DialogStatus_PlayingPrimary:
            if (!IsPrimaryHandle(handle)) {
                return 0;
            }
            StopDialogEntryVoice(FindDialogEntry(g_primaryDialogHandle));
            ResetDialogInfoAndState(0, DialogStatus_Idle);
            return 1;

        case DialogStatus_PlayingPrimaryWithSecondary:
            if (IsPrimaryHandle(handle)) {
                StopDialogEntryVoice(FindDialogEntry(g_primaryDialogHandle));
                UpgradeDialogInfoAndState(DialogStatus_LoadRequestedPrimary);
            }
            else {
                UpdateDialogState(DialogStatus_PlayingPrimaryState13);
            }
            return 1;

        case DialogStatus_PlayingPrimaryState12:
            if (IsPrimaryHandle(handle)) {
                StopDialogEntryVoice(FindDialogEntry(g_primaryDialogHandle));
                UpgradeDialogInfoAndState(DialogStatus_LoadDeferredPrimary);
            }
            else {
                UpdateDialogState(DialogStatus_PlayingPrimaryState13);
            }
            return 1;

        case DialogStatus_PlayingPrimaryState13:
            if (!IsPrimaryHandle(handle)) {
                return 0;
            }
            StopDialogEntryVoice(FindDialogEntry(g_primaryDialogHandle));
            UpgradeDialogInfoAndState(DialogStatus_KillRequestedPrimary);
            return 1;

        case DialogStatus_PlayingPrimaryState14:
            if (IsPrimaryHandle(handle)) {
                StopDialogEntryVoice(FindDialogEntry(g_primaryDialogHandle));
                UpgradeToPrimaryState();
            }
            else {
                ResetDialogInfoAndState(1, DialogStatus_PlayingPrimary);
            }
            return 1;

        default:
            return 1;
    }
}

static DialogEntry* FindDialogEntry(s32 handle) {
    if (handle == 0) {
        return nullptr;
    }

    for (s32 i = 0; i < DIALOG_ENTRY_COUNT; ++i) {
        if (g_dialogEntries[i].valid && g_dialogEntries[i].handle == handle) {
            return &g_dialogEntries[i];
        }
    }

    return nullptr;
}

static bool IsDialogEntryPlaying(const DialogEntry* entry) {
    return entry
        && entry->voice != AUDIO_VOICE_INVALID
        && AudioEngine::IsVoicePlaying(entry->voice);
}

static void StopDialogEntryVoice(DialogEntry* entry) {
    if (!entry || entry->voice == AUDIO_VOICE_INVALID) {
        return;
    }

    AudioEngine::StopVoice(entry->voice);
    entry->voice = AUDIO_VOICE_INVALID;
}

static void UpdateActiveDialogVoiceVolumes(f32 volume) {
    for (s32 i = 0; i < DIALOG_ENTRY_COUNT; ++i) {
        DialogEntry& entry = g_dialogEntries[i];
        if (entry.valid && entry.voice != AUDIO_VOICE_INVALID) {
            AudioEngine::SetVoiceVolume(entry.voice, volume);
        }
    }
}

static void ReleaseDialogEntry(DialogEntry* entry) {
    if (!entry) {
        return;
    }

    StopDialogEntryVoice(entry);
    if (entry->sample != AUDIO_SAMPLE_INVALID) {
        AudioEngine::UnloadSample(entry->sample);
        entry->sample = AUDIO_SAMPLE_INVALID;
    }

    entry->valid = false;
}

static void ClearDialogPendingLoadState() {
    g_dialogLoadPending = 0;
    g_dialogLoadPendingState = -1;
    g_dialogLoadCompletionState = -1;
    g_dialogLoadDueFrame = -1;
}

// PSX: CheckFlushCount__F5Queue (JCSDLG.CPP:1859, 0x80043700)
static void CheckDialogFlushCount(s32 queue) {
    if (queue < 0 || queue >= 2) {
        return;
    }

    DialogQueueInfo& q = g_dialogQueues[queue];
    if (q.flushFrame == 0) {
        return;
    }

    const s32 frameCounter = GetDialogFrameCounter();
    if (q.flushFrame > frameCounter - 1200) {
        return;
    }

    q.flushFrame = frameCounter;
    if (queue == 0 && g_dialogQueues[0].priority <= 0x3FFFFFFF) {
        KillDialogByHandleInternal(g_primaryDialogHandle);
    }
}

// PSX: DialogTask states 5..9 (JCSDLG.CPP:1977..2014, 0x80043808)
static void ProcessDialogLoadedStates() {
    if (g_dialogStatus < DialogStatus_LoadedPrimary || g_dialogStatus > DialogStatus_LoadedPrimaryAndSecondary) {
        return;
    }

    DialogQueueInfo& q0 = g_dialogQueues[0];
    if (q0.timeoutFrame != 0) {
        const s32 frameCounter = GetDialogFrameCounter();
        if (q0.timeoutFrame < frameCounter) {
            KillDialogByHandleInternal(q0.handle);
            SetDlgStatus(12);
        }
        else {
            const DialogQueuePosition& qPos = g_dialogQueuePos[0];
            PlayDialogRequest(q0.handle, q0.playPosition, qPos.hasPos ? &qPos.pos : nullptr, 0u);
        }
    }
    else {
        CheckDialogFlushCount(0);
    }

    if (g_dialogStatus == DialogStatus_LoadedPrimaryAndSecondary) {
        DialogQueueInfo& q1 = g_dialogQueues[1];
        if (q1.timeoutFrame != 0) {
            const s32 frameCounter = GetDialogFrameCounter();
            if (q1.timeoutFrame < frameCounter) {
                KillDialogByHandleInternal(q1.handle);
            }
            else {
                const DialogQueuePosition& q1Pos = g_dialogQueuePos[1];
                PlayDialogRequest(q1.handle, q1.playPosition, q1Pos.hasPos ? &q1Pos.pos : nullptr, 0u);
            }
        }
        else {
            CheckDialogFlushCount(1);
        }
    }
}

static void ReleaseNonSlotDialogEntries() {
    for (s32 i = 0; i < DIALOG_ENTRY_COUNT; ++i) {
        DialogEntry& entry = g_dialogEntries[i];
        if (!entry.valid) {
            continue;
        }

        if (entry.handle == g_primaryDialogHandle || entry.handle == g_secondaryDialogHandle) {
            continue;
        }

        ReleaseDialogEntry(&entry);
    }
}

static void ProcessDialogPlayingStates() {
    if (g_dialogStatus < DialogStatus_PlayingPrimary || g_dialogStatus > DialogStatus_PlayingPrimaryState14) {
        return;
    }

    DialogEntry* primaryEntry = FindDialogEntry(g_primaryDialogHandle);
    if (primaryEntry && IsDialogEntryPlaying(primaryEntry)) {
        return;
    }

    switch (g_dialogStatus) {
        case DialogStatus_PlayingPrimary:
            ResetDialogInfoAndState(0, DialogStatus_Idle);
            break;

        case DialogStatus_PlayingPrimaryWithSecondary:
            UpgradeDialogInfoAndState(DialogStatus_LoadRequestedPrimary);
            break;

        case DialogStatus_PlayingPrimaryState12:
            UpgradeDialogInfoAndState(DialogStatus_LoadDeferredPrimary);
            break;

        case DialogStatus_PlayingPrimaryState14:
            UpgradeToPrimaryState();
            break;

        case DialogStatus_PlayingPrimaryState13:
        default:
            UpgradeDialogInfoAndState(DialogStatus_KillRequestedPrimary);
            break;
    }
}

// PSX: DialogTask__FP6_RTASK (JCSDLG.CPP:1957, 0x80043808)
static void ProcessDialogTask() {
#if defined(RC_PLATFORM_PSP)
    FinishDialogAsyncLoad(false);
#endif
    CompletePendingDialogLoad(false);

    ProcessDialogLoadedStates();
    ProcessDialogPlayingStates();

    if (g_dialogStatus == DialogStatus_PlayingPrimaryState14) {
        CheckDialogFlushCount(1);
    }
}

// PC: services the dialog state machine every frame. PSX's StartLoad
// (JCSDLG.CPP:3208) issues a real async CD read and registers CDDoneCallback
// via rCDSetCallBack - the CD controller's own interrupt fires it as soon as
// the real disc read finishes, entirely decoupled from DialogTask's 16-tick
// RTASK schedule (DialogTask only does status-driven housekeeping: retry a
// deferred play once loaded, notice a finished voice). PC's decode is
// synchronous/instant, so the closest match to that interrupt is servicing the
// pending-load completion every frame instead of waiting for DialogTask's next
// tick (up to 16 frames later) - which was starving short timeouts and
// short-lived per-frame-retried callers. DialogTask still runs on its own
// 16-tick schedule too; calling both is harmless; this just makes the loaded
// transition (and the retry/finish checks that depend on it) prompt.
void jcsUpdateDialogCD() {
    ProcessDialogTask();
}

// PSX: DialogTask__FP6_RTASK (JCSDLG.CPP:1957, 0x80043808)
static s32 DialogTask(RTASK* task) {
    (void)task;
    MARKFUNCTION(0x80043808);

    ProcessDialogTask();
    return 16;
}

// PSX: jcsInitializeDialog__Fv (JCSDLG.CPP:387, 0x800428C4)
static RTASK* jcsInitializeDialog() {
    MARKFUNCTION(0x800428C4);

    if (!g_dialogTask) {
        g_dialogTask = rNewTask(&rMainTaskList, DialogTask, 16);
    }
    return g_dialogTask;
}

// PSX: jcsTerminateDialog__Fv (JCSDLG.CPP, paired with dialog init task)
static void jcsTerminateDialog() {
    if (g_dialogTask) {
        rDelTask(&rMainTaskList, g_dialogTask);
        g_dialogTask = nullptr;
    }
}

static void UpdateDialogStatusFromSlots() {
    if (g_dialogStatusUpdateInProgress) {
        return;
    }

    g_dialogStatusUpdateInProgress = true;

    ProcessDialogTask();

    DialogEntry* primaryEntry = FindDialogEntry(g_primaryDialogHandle);
    DialogEntry* secondaryEntry = FindDialogEntry(g_secondaryDialogHandle);

    if (!primaryEntry) {
        g_primaryDialogHandle = 0;
        g_primaryDialogPriority = -1;
        ResetDialogQueueInfo(0);
    }

    if (!secondaryEntry) {
        g_secondaryDialogHandle = 0;
        g_secondaryDialogPriority = -1;
        ResetDialogQueueInfo(1);
    }

    ReleaseNonSlotDialogEntries();
    g_dialogStatusUpdateInProgress = false;
}

static bool IsDialogDeferredLoadState(s32 pendingState, s32 currentState) {
    return (pendingState == DialogStatus_LoadRequestedPrimary
            && currentState == DialogStatus_LoadDeferredPrimary)
        || (pendingState == DialogStatus_LoadRequestedPrimary
            && currentState == DialogStatus_LoadDeferredSecondary)
        || (pendingState == DialogStatus_LoadedPrimaryWithDeferred
            && currentState == DialogStatus_LoadedPrimaryWithDeferredFromState2)
        || (pendingState == DialogStatus_PlayingPrimaryWithSecondary
            && currentState == DialogStatus_PlayingPrimaryState12);
}

static s32 DialogFallbackStateAfterDeferredLoad(s32 pendingState) {
    switch (pendingState) {
        case DialogStatus_LoadRequestedPrimary:
            return DialogStatus_Idle;

        case DialogStatus_LoadedPrimaryWithDeferred:
            return DialogStatus_LoadedPrimary;

        case DialogStatus_PlayingPrimaryWithSecondary:
            return DialogStatus_PlayingPrimary;

        default:
            return DialogStatus_Idle;
    }
}

static void LoadDialogToAudioMemory() {
    ClearDialogPendingLoadState();
    UpdateDialogState(DialogStatus_LoadedPrimary);
}

// PSX: CDDoneDefer__F5Queue5StateT1 (JCSDLG.CPP:1819, 0x80043684)
static void CDDoneDefer(s32 queue, s32 reloadState, s32 fallbackState) {
    const s32 existingHandle = g_dialogQueues[queue].handle;
    const s32 priority = g_dialogQueues[queue].priority;
    const s32 deferredCharacter = g_deferredDialogCharacter;
    const s32 deferredDialogId = g_deferredDialogId;

    ClearDialogPendingLoadState();
    if (!RequestLoadDialog(existingHandle, deferredCharacter, deferredDialogId,
                           priority, queue, reloadState)) {
        UpdateDialogState(fallbackState);
    }
}

// PSX: CDDoneCallback__Flll (JCSDLG.CPP:1700, 0x80043568)
static void CDDoneCallback() {
    // PSX has no "is a load still pending" guard here - it reads gp+1212
    // (current status) and dispatches unconditionally on whatever it is right
    // now. A guard on g_dialogLoadPending is wrong: that flag is legitimately
    // cleared by unrelated state transitions (e.g. a secondary being promoted
    // to primary) that can happen before this fires, and dropping the call in
    // that case permanently strands the state machine instead of letting the
    // switch's own state-specific handling (or its default no-op) apply.
    switch (g_dialogStatus) {
        case DialogStatus_LoadRequestedPrimary:
            LoadDialogToAudioMemory();
            g_dialogQueues[0].flushFrame = GetDialogFrameCounter();
            break;

        case DialogStatus_LoadDeferredPrimary:
            CDDoneDefer(0, DialogStatus_LoadRequestedPrimary, DialogStatus_Idle);
            break;

        case DialogStatus_KillRequestedPrimary:
            ResetDialogInfoAndState(0, DialogStatus_Idle);
            break;

        case DialogStatus_LoadDeferredSecondary:
            UpgradeDialogInfoAndState(DialogStatus_LoadDeferredPrimary);
            CDDoneDefer(0, DialogStatus_LoadRequestedPrimary, DialogStatus_Idle);
            break;

        case DialogStatus_LoadedPrimaryWithDeferred:
            ClearDialogPendingLoadState();
            UpdateDialogState(DialogStatus_LoadedPrimaryAndSecondary);
            g_dialogQueues[0].flushFrame = GetDialogFrameCounter();
            break;

        case DialogStatus_LoadedPrimaryWithDeferredFromState2:
            CDDoneDefer(1, DialogStatus_LoadedPrimaryWithDeferred,
                        DialogStatus_LoadedPrimary);
            break;

        case DialogStatus_LoadedPrimaryWithDeferredFromState3:
            ResetDialogInfoAndState(1, DialogStatus_LoadedPrimary);
            break;

        case DialogStatus_PlayingPrimaryWithSecondary:
            ClearDialogPendingLoadState();
            UpdateDialogState(DialogStatus_PlayingPrimaryState14);
            break;

        case DialogStatus_PlayingPrimaryState12:
            CDDoneDefer(1, DialogStatus_PlayingPrimaryWithSecondary,
                        DialogStatus_PlayingPrimary);
            break;

        case DialogStatus_PlayingPrimaryState13:
            ResetDialogInfoAndState(1, DialogStatus_PlayingPrimary);
            break;

        default:
            break;
    }
}

// PSX: IsPrimaryHandle__Fl
static s32 IsPrimaryHandle(s32 handle) {
    if (handle <= 0 || handle != g_primaryDialogHandle) {
        return 0;
    }
    return (handle != g_secondaryDialogHandle) ? 1 : 0;
}

// PSX: IsSecondaryHandle__Fl
static s32 IsSecondaryHandle(s32 handle) {
    if (handle <= 0 || handle == g_primaryDialogHandle) {
        return 0;
    }
    return (handle == g_secondaryDialogHandle) ? 1 : 0;
}

// PSX: IsEitherHandle__Fl
static s32 IsEitherHandle(s32 handle) {
    if (IsPrimaryHandle(handle)) {
        return 1;
    }
    return IsSecondaryHandle(handle);
}

// PSX: IsCurrentHandle__Fl
static s32 IsCurrentHandle(s32 handle) {
    if (handle <= 0) {
        return 0;
    }

    switch (g_dialogStatus) {
        case 1:
        case 2:
        case 3:
        case 5:
        case 10:
            return IsPrimaryHandle(handle);

        case 4:
        case 6:
        case 7:
        case 8:
        case 9:
        case 11:
        case 12:
        case 13:
        case 14:
            return IsEitherHandle(handle);

        default:
            return 0;
    }
}

static DialogEntry* AllocateDialogEntry() {
    for (s32 i = 0; i < DIALOG_ENTRY_COUNT; ++i) {
        if (!g_dialogEntries[i].valid) {
            g_dialogEntries[i].handle = g_nextDialogHandle++;
            if (g_nextDialogHandle <= 0) {
                g_nextDialogHandle = 1;
            }
            g_dialogEntries[i].sample = AUDIO_SAMPLE_INVALID;
            g_dialogEntries[i].voice = AUDIO_VOICE_INVALID;
            g_dialogEntries[i].valid = true;
#if defined(RC_PLATFORM_PSP)
            g_dialogEntries[i].asyncSerial = 0;
#endif
            return &g_dialogEntries[i];
        }
    }

    DialogEntry* entry = &g_dialogEntries[0];
    if (entry->voice != AUDIO_VOICE_INVALID) {
        AudioEngine::StopVoice(entry->voice);
    }
    if (entry->sample != AUDIO_SAMPLE_INVALID) {
        AudioEngine::UnloadSample(entry->sample);
    }
    entry->handle = g_nextDialogHandle++;
    if (g_nextDialogHandle <= 0) {
        g_nextDialogHandle = 1;
    }
    entry->sample = AUDIO_SAMPLE_INVALID;
    entry->voice = AUDIO_VOICE_INVALID;
    entry->valid = true;
#if defined(RC_PLATFORM_PSP)
    entry->asyncSerial = 0;
#endif
    return entry;
}

// PSX: PlayLoaded__FPC10tagLVector5State (JCSDLG.CPP:2664, 0x8004406C)
static s32 PlayDialogHandle(DialogEntry* entry, const LVector* posPtr) {
    if (!entry || !entry->valid || !g_sound) {
        return 0;
    }

#if defined(RC_PLATFORM_PSP)
    if (entry->asyncSerial != 0) {
        FinishDialogAsyncLoad(true);
    }
#endif
    if (entry->sample == AUDIO_SAMPLE_INVALID) {
        return 0;
    }

    if (entry->voice != AUDIO_VOICE_INVALID) {
        if (AudioEngine::IsVoicePlaying(entry->voice)) {
            return 1;
        }

        AudioEngine::StopVoice(entry->voice);
        entry->voice = AUDIO_VOICE_INVALID;
    }

    f32 pan = 0.0f;
    f32 vol = g_sound->dialogVolume;
    if (posPtr) {
        u16 volL = 0;
        u16 volR = 0;
        rsdWorld::ComputeObjectVolumes(0xFFFFu, posPtr, volL, volR, 5000);
        const f32 fVolL = PsxVolToFloat(volL);
        const f32 fVolR = PsxVolToFloat(volR);
        vol = (fVolL + fVolR) * 0.5f * g_sound->dialogVolume;
        if (fVolL + fVolR > 0.0f) {
            pan = (fVolR - fVolL) / (fVolL + fVolR);
        }
    }

    // PSX plays every line through the locked gp+1220 voice; fall back to a
    // top-priority pool voice if the reservation is unavailable.
    if (g_dialogReservedVoice != AUDIO_VOICE_INVALID) {
        entry->voice = AudioEngine::PlayOnReservedVoice(g_dialogReservedVoice, entry->sample, vol, pan);
    }
    if (entry->voice == AUDIO_VOICE_INVALID) {
        entry->voice = AudioEngine::PlaySample(entry->sample, vol, pan, false, AUDIO_PRIO_DIALOGUE);
    }

    if (entry->voice == AUDIO_VOICE_INVALID) {
        if (DialogTraceEnabled()) {
            LOG("[DialogTrace] AUDIO-START-FAILED handle=%d (voice pool exhausted?)", entry->handle);
        }
        return 0;
    }

    if (DialogTraceEnabled()) {
        LOG("[DialogTrace] AUDIO-START handle=%d voice=%u", entry->handle, entry->voice);
    }
    return 1;
}

// PSX: PlayLoaded__FPC10tagLVector5State (JCSDLG.CPP:2664, 0x8004406C)
static s32 PlayLoadedState(s32 playPosition, const LVector* posPtr, s32 nextState) {
    DialogEntry* primaryEntry = FindDialogEntry(g_primaryDialogHandle);
    if (!primaryEntry || !IsPrimaryHandle(primaryEntry->handle)) {
        return 0;
    }

    const u16 clipEntry = g_dialogQueues[0].clipEntry;
    if (clipEntry == 0) {
        KillDialogByHandleInternal(g_primaryDialogHandle);
        return 0;
    }

    const bool clipAlreadyUsed = (DialogHeaderU8((u32)clipEntry) & 0x80) != 0;
    if (clipEntry == g_lastDialogChoiceOffset || clipAlreadyUsed) {
        if (DialogTraceEnabled()) {
            LOG("[DialogTrace] PLAY-GUARD kill clipEntry=%u lastChoice=%u alreadyUsed=%d headerByte=0x%02X",
                clipEntry, g_lastDialogChoiceOffset, clipAlreadyUsed ? 1 : 0, DialogHeaderU8((u32)clipEntry));
        }
        KillDialogByHandleInternal(g_primaryDialogHandle);
        return 0;
    }

    // JCSDLG keeps only one active dialog voice at a time.
    for (s32 i = 0; i < DIALOG_ENTRY_COUNT; ++i) {
        DialogEntry& other = g_dialogEntries[i];
        if (!other.valid || other.handle == primaryEntry->handle) {
            continue;
        }
        StopDialogEntryVoice(&other);
    }

    const s32 started = PlayDialogHandle(primaryEntry, posPtr);
    if (started == 0) {
        KillDialogByHandleInternal(g_primaryDialogHandle);
        return 0;
    }

    g_dialogStatus = nextState;
    if ((u32)clipEntry < g_dialogHeader.size()) {
        g_dialogHeader[clipEntry] = (u8)(g_dialogHeader[clipEntry] | 0x80);
    }
    g_lastDialogChoiceOffset = clipEntry;
    g_dialogQueues[0].clipEntry = 0;
    g_dialogQueues[0].playPosition = playPosition;
    g_dialogQueues[0].timeoutFrame = 0;
    ClearDialogPendingLoadState();
    return 1;
}

// PSX: jcsPlayDialog__FlPC10tagLVectorUl (JCSDLG.CPP:1121, 0x80043044)
static s32 PlayDialogRequest(s32 handleParam, s32 playPosition, const LVector* posPtr, u32 timeout) {
    if (DialogTraceEnabled()) {
        LOG("[DialogTrace] play req handleParam=%d resolved=%d timeout=%u state=%d primary=%d secondary=%d",
            handleParam, ResolveDialogHandleParam(handleParam), timeout,
            g_dialogStatus, g_primaryDialogHandle, g_secondaryDialogHandle);
    }

    SetDlgStatus(0);

    if (g_dialogSystemStarted == 0 || !g_sound) {
        SetDlgStatus(1);
        return 0;
    }

    const s32 handle = ResolveDialogHandleParam(handleParam);
    if (!jcsValidateHandle(handle)) {
        SetDlgStatus(6);
        return 0;
    }

    switch (g_dialogStatus) {
        case DialogStatus_Idle:
        case DialogStatus_KillRequestedPrimary:
        case DialogStatus_LoadDeferredSecondary:
            SetDlgStatus(7);
            return 0;

        case DialogStatus_LoadRequestedPrimary:
        case DialogStatus_LoadDeferredPrimary:
            return UpdateDialogTimeout(timeout, playPosition, posPtr, 0);

        case DialogStatus_LoadedPrimary:
            return PlayLoadedState(playPosition, posPtr, DialogStatus_PlayingPrimary);

        case DialogStatus_LoadedPrimaryWithDeferred:
            if (!IsPrimaryHandle(handle)) {
                return UpdateDialogTimeout(timeout, playPosition, posPtr, 1);
            }
            return PlayLoadedState(playPosition, posPtr, DialogStatus_PlayingPrimaryWithSecondary);

        case DialogStatus_LoadedPrimaryWithDeferredFromState2:
            if (!IsPrimaryHandle(handle)) {
                return UpdateDialogTimeout(timeout, playPosition, posPtr, 1);
            }
            return PlayLoadedState(playPosition, posPtr, DialogStatus_PlayingPrimaryState12);

        case DialogStatus_LoadedPrimaryWithDeferredFromState3:
            if (IsSecondaryHandle(handle)) {
                SetDlgStatus(7);
                return 0;
            }
            return PlayLoadedState(playPosition, posPtr, DialogStatus_PlayingPrimaryState13);

        case DialogStatus_LoadedPrimaryAndSecondary:
            if (IsPrimaryHandle(handle)) {
                return PlayLoadedState(playPosition, posPtr, DialogStatus_PlayingPrimaryState14);
            }
            KillDialogByHandleInternal(g_primaryDialogHandle);
            return PlayLoadedState(playPosition, posPtr, DialogStatus_PlayingPrimary);

        case DialogStatus_PlayingPrimaryWithSecondary:
        case DialogStatus_PlayingPrimaryState12:
        case DialogStatus_PlayingPrimaryState14:
            if (!IsSecondaryHandle(handle)) {
                return 1;
            }
            return UpdateDialogTimeout(timeout, playPosition, posPtr, 1);

        case DialogStatus_PlayingPrimaryState13:
            if (IsSecondaryHandle(handle)) {
                SetDlgStatus(7);
                return 0;
            }
            return 1;

        default:
            return 1;
    }
}

struct DialogLoadInfo {
    DialogClipSelection selection;
    u32 loadBytes = 0;
};

static u32 RoundDialogTransferSize(u32 bytes) {
    if ((bytes & (DIALOG_SECTOR_SIZE - 1u)) == 0u) {
        return bytes;
    }
    return (bytes & ~(DIALOG_SECTOR_SIZE - 1u)) + DIALOG_SECTOR_SIZE;
}

// PSX: ConflictWithOtherQueue__FUs5Queue compares against queue 1's selected clip.
static bool ConflictWithOtherQueue(u16 clipEntry, s32 queue) {
    (void)queue;
    return clipEntry != 0 && clipEntry == g_dialogQueues[1].clipEntry;
}

// PSX: IfCanLoad__F11rsCharacter8rsDialogPUlT2
static u16 IfCanLoadDialog(s32 character, s32 dialogId, DialogLoadInfo* outLoadInfo) {
    if (!outLoadInfo) {
        return 0;
    }

    DialogLoadInfo info;
    if (!SelectDialogClip(character, dialogId, &info.selection)) {
        return 0;
    }

    info.loadBytes = RoundDialogTransferSize(info.selection.ByteSize());
    *outLoadInfo = info;
    return info.selection.choiceOffset;
}

// PSX: IsLoadableDialog__F11rsCharacter8rsDialog
static bool IsLoadableDialog(s32 character, s32 dialogId) {
    DialogLoadInfo info;
    const u16 clipEntry = IfCanLoadDialog(character, dialogId, &info);
    return clipEntry != 0 && clipEntry != g_dialogQueues[0].clipEntry;
}

static s32 DialogCompletionStateForLoadState(s32 state) {
    switch (state) {
        case DialogStatus_LoadRequestedPrimary:
            return DialogStatus_LoadedPrimary;

        case DialogStatus_LoadedPrimaryWithDeferred:
            return DialogStatus_LoadedPrimaryAndSecondary;

        case DialogStatus_PlayingPrimaryWithSecondary:
            return DialogStatus_PlayingPrimaryState14;

        default:
            return state;
    }
}

static void BeginDialogPendingLoad(s32 pendingState) {
    g_dialogLoadPending = 1;
    g_dialogLoadPendingState = pendingState;
    g_dialogLoadCompletionState = DialogCompletionStateForLoadState(pendingState);
    g_dialogLoadDueFrame = GetDialogFrameCounter() + kDialogLoadLatencyFrames;
    ++g_dialogLoadSerial;
    if (g_dialogLoadSerial == 0) {
        ++g_dialogLoadSerial;
    }
}

static bool CompletePendingDialogLoad(bool force) {
    if (g_dialogLoadPending == 0) {
        return false;
    }

    const s32 frameCounter = GetDialogFrameCounter();
    if (!force && g_dialogLoadDueFrame >= 0 && frameCounter < g_dialogLoadDueFrame) {
        // rFrameCount60 is reset by menu/loading transitions. A due frame from
        // before that reset must be ready on the next task tick, not delayed
        // until the old absolute frame is reached again.
        if (g_dialogLoadDueFrame - frameCounter <= kDialogLoadLatencyFrames) {
            return false;
        }
    }

#if defined(RC_PLATFORM_PSP)
    // The clip is still being read: complete on a later task tick.
    if (!FinishDialogAsyncLoad(force)) {
        return false;
    }
#endif

    const u32 completedSerial = g_dialogLoadSerial;
    if (DialogTraceEnabled()) {
        LOG("[DialogTrace] CD-DONE serial=%u state=%d pendingState=%d completionState=%d force=%d",
            completedSerial, g_dialogStatus, g_dialogLoadPendingState,
            g_dialogLoadCompletionState, force ? 1 : 0);
    }

    CDDoneCallback();
    if (g_dialogLoadPending != 0 && g_dialogLoadSerial == completedSerial) {
        ClearDialogPendingLoadState();
    }
    return true;
}

static void ReleaseDialogQueueSlot(s32 queue, s32 keepHandle) {
    if (queue == 0) {
        if (g_primaryDialogHandle != 0 && g_primaryDialogHandle != keepHandle) {
            ReleaseDialogEntry(FindDialogEntry(g_primaryDialogHandle));
        }
        g_primaryDialogHandle = 0;
        g_primaryDialogPriority = -1;
    }
    else if (queue == 1) {
        if (g_secondaryDialogHandle != 0 && g_secondaryDialogHandle != keepHandle) {
            ReleaseDialogEntry(FindDialogEntry(g_secondaryDialogHandle));
        }
        g_secondaryDialogHandle = 0;
        g_secondaryDialogPriority = -1;
    }

    ResetDialogQueueInfo(queue);
}

static void AssignDialogEntryToQueue(DialogEntry* entry, s32 queue, u16 clipEntry, u32 sectors) {
    if (!entry || queue < 0 || queue >= 2) {
        return;
    }

    ReleaseDialogQueueSlot(queue, entry->handle);

    DialogQueueInfo& q = g_dialogQueues[queue];
    q.priority = entry->priority;
    q.handle = entry->handle;
    q.sectors = static_cast<s32>(sectors);
    q.timeoutFrame = 0;
    q.playPosition = 0;
    q.clipEntry = clipEntry;
    q.flushFrame = 0;
    q.dialogId = entry->dialogId;

    if (queue == 0) {
        g_primaryDialogHandle = entry->handle;
        g_primaryDialogPriority = entry->priority;
    }
    else {
        g_secondaryDialogHandle = entry->handle;
        g_secondaryDialogPriority = entry->priority;
    }
}

// PSX: CreateHandle__F8rsDialogl5Queue
static s32 CreateDialogHandle(s32 character, s32 dialogId, s32 priority, s32 queue) {
    DialogEntry* entry = AllocateDialogEntry();
    if (!entry) {
        return 0;
    }

    entry->character = character;
    entry->dialogId = dialogId;
    entry->priority = priority;
    entry->sample = AUDIO_SAMPLE_INVALID;
    entry->voice = AUDIO_VOICE_INVALID;
    AssignDialogEntryToQueue(entry, queue, 0, 0);
    return entry->handle;
}

// PSX: StartLoad__Fl8rsDialoglUlUsUl5Queue5State
static s32 StartLoadDialog(s32 existingHandle, s32 character, s32 dialogId, s32 priority,
                           const DialogLoadInfo& loadInfo, s32 queue, s32 state) {
    if (queue < 0 || queue >= 2) {
        return 0;
    }

    DialogEntry* entry = FindDialogEntry(existingHandle);
    if (!entry) {
        entry = AllocateDialogEntry();
    }
    if (!entry) {
        SetDlgStatus(11);
        return 0;
    }

    StopDialogEntryVoice(entry);
    if (entry->sample != AUDIO_SAMPLE_INVALID) {
        AudioEngine::UnloadSample(entry->sample);
        entry->sample = AUDIO_SAMPLE_INVALID;
    }

    entry->character = character;
    entry->dialogId = dialogId;
    entry->priority = priority;
#if defined(RC_PLATFORM_PSP)
    entry->asyncSerial = 0;
    // The sample is attached when the worker finishes, before the pending
    // CD completion below is allowed to fire.
    const bool loadingAsync = SubmitDialogAsyncLoad(entry, loadInfo.selection);
    if (!loadingAsync) {
        entry->sample = LoadDialogSampleFromSelection(character, dialogId, loadInfo.selection);
    }
    if (!loadingAsync && entry->sample == AUDIO_SAMPLE_INVALID) {
#else
    entry->sample = LoadDialogSampleFromSelection(character, dialogId, loadInfo.selection);
    if (entry->sample == AUDIO_SAMPLE_INVALID) {
#endif
        ReleaseDialogEntry(entry);
        SetDlgStatus(11);
        return 0;
    }

    AssignDialogEntryToQueue(entry, queue, loadInfo.selection.choiceOffset, loadInfo.selection.sectors);
    UpdateDialogState(state);
    BeginDialogPendingLoad(state);
    SetDlgStatus(0);
    return entry->handle;
}

// PSX: RequestLoad__Fl11rsCharacter8rsDialogl5Queue5State
static s32 RequestLoadDialog(s32 existingHandle, s32 character, s32 dialogId,
                             s32 priority, s32 queue, s32 state) {
    DialogLoadInfo loadInfo;
    const u16 clipEntry = IfCanLoadDialog(character, dialogId, &loadInfo);
    if (clipEntry == 0 || ConflictWithOtherQueue(clipEntry, queue)) {
        return 0;
    }

    return StartLoadDialog(existingHandle, character, dialogId, priority, loadInfo, queue, state);
}

// PSX: PrepareDefer__F11rsCharacter8rsDialogl5State5Queue
static s32 PrepareDeferDialog(s32 character, s32 dialogId, s32 priority, s32 state, s32 queue) {
    g_deferredDialogCharacter = character;
    g_deferredDialogId = dialogId;
    const s32 handle = CreateDialogHandle(character, dialogId, priority, queue);
    UpdateDialogState(state);
    if (!IsDialogDeferredLoadState(g_dialogLoadPendingState, state)) {
        ClearDialogPendingLoadState();
    }
    return handle;
}

// PSX: LoadAllReady__F11rsCharacter8rsDialogl
static s32 LoadAllReadyDialog(s32 character, s32 dialogId, s32 priority) {
    DialogLoadInfo loadInfo;
    const u16 clipEntry = IfCanLoadDialog(character, dialogId, &loadInfo);
    if (clipEntry == 0) {
        return 0;
    }

    if (priority <= 0) {
        SetDlgStatus(13);
        return 0;
    }

    s32 queue = 0;
    const s32 primaryPriority = g_dialogQueues[0].priority;
    const s32 secondaryPriority = g_dialogQueues[1].priority;
    if (primaryPriority < priority) {
        if (secondaryPriority < priority) {
            queue = (primaryPriority < secondaryPriority) ? 0 : 1;
        }
        else {
            queue = 1;
        }
    }
    else if (secondaryPriority < priority) {
        queue = 1;
    }
    else {
        SetDlgStatus(13);
        return 0;
    }

    if (ConflictWithOtherQueue(clipEntry, queue)) {
        return 0;
    }

    KillDialogByHandleInternal(g_dialogQueues[queue].handle);
    // PSX LoadAllReady uses the computed queue for the conflict check and the
    // kill above, but hardcodes the StartLoad destination to queue 1 - so a
    // queue-0 eviction leaves queue 0 empty and the new dialog lands in
    // queue 1 anyway. Reproduced verbatim (SLUS_006.84_full.c:75681-75688).
    return StartLoadDialog(0, character, dialogId, priority, loadInfo, 1,
                           DialogStatus_LoadedPrimaryWithDeferred);
}

// PSX: jcsLoadDialog__F11rsCharacter8rsDialogl (JCSDLG.CPP:772, 0x80042C78)
static s32 jcsLoadDialog(s32 character, s32 dialogId, s32 priority) {
    MARKFUNCTION(0x80042C78);

    if (DialogTraceEnabled()) {
        LOG("[DialogTrace] load req char=%d dialog=%d priority=%d state=%d primary=%d secondary=%d started=%d",
            character, dialogId, priority, g_dialogStatus,
            g_primaryDialogHandle, g_secondaryDialogHandle, g_dialogSystemStarted);
    }

    SetDlgStatus(0);
    if (g_dialogSystemStarted == 0 || !g_sound) {
        SetDlgStatus(1);
        return 0;
    }

    switch (g_dialogStatus) {
        case DialogStatus_Idle:
            return RequestLoadDialog(0, character, dialogId, priority, 0,
                                     DialogStatus_LoadRequestedPrimary);

        case DialogStatus_LoadRequestedPrimary:
        case DialogStatus_LoadDeferredPrimary:
            if (priority > 0x3FFFFFFF && IsLoadableDialog(character, dialogId)) {
                return PrepareDeferDialog(character, dialogId, priority,
                                          DialogStatus_LoadDeferredPrimary, 0);
            }
            SetDlgStatus(3);
            return 0;

        case DialogStatus_KillRequestedPrimary:
            return PrepareDeferDialog(character, dialogId, priority,
                                      DialogStatus_LoadDeferredSecondary, 1);

        case DialogStatus_LoadDeferredSecondary:
            if (priority > 0 && IsLoadableDialog(character, dialogId)) {
                return PrepareDeferDialog(character, dialogId, priority,
                                          DialogStatus_LoadDeferredSecondary, 1);
            }
            SetDlgStatus(3);
            return 0;

        case DialogStatus_LoadedPrimary:
            return RequestLoadDialog(0, character, dialogId, priority, 1,
                                     DialogStatus_LoadedPrimaryWithDeferred);

        case DialogStatus_LoadedPrimaryWithDeferred:
        case DialogStatus_LoadedPrimaryWithDeferredFromState2:
            if (priority > 0 && IsLoadableDialog(character, dialogId)) {
                return PrepareDeferDialog(character, dialogId, priority,
                                          DialogStatus_LoadedPrimaryWithDeferredFromState2, 1);
            }
            SetDlgStatus(3);
            return 0;

        case DialogStatus_LoadedPrimaryWithDeferredFromState3:
            return PrepareDeferDialog(character, dialogId, priority,
                                      DialogStatus_LoadedPrimaryWithDeferredFromState2, 1);

        case DialogStatus_LoadedPrimaryAndSecondary:
            return LoadAllReadyDialog(character, dialogId, priority);

        case DialogStatus_PlayingPrimary:
            return RequestLoadDialog(0, character, dialogId, priority, 1,
                                     DialogStatus_PlayingPrimaryWithSecondary);

        case DialogStatus_PlayingPrimaryWithSecondary:
        case DialogStatus_PlayingPrimaryState12:
        case DialogStatus_PlayingPrimaryState13:
            if (priority > 0 && IsLoadableDialog(character, dialogId)) {
                return PrepareDeferDialog(character, dialogId, priority,
                                          DialogStatus_PlayingPrimaryState12, 1);
            }
            SetDlgStatus(3);
            return 0;

        case DialogStatus_PlayingPrimaryState14:
        {
            if (priority <= g_dialogQueues[1].priority) {
                SetDlgStatus(13);
                return 0;
            }

            DialogLoadInfo loadInfo;
            const u16 clipEntry = IfCanLoadDialog(character, dialogId, &loadInfo);
            if (clipEntry == 0) {
                SetDlgStatus(13);
                return 0;
            }

            if (clipEntry == g_dialogQueues[0].clipEntry) {
                SetDlgStatus(13);
                return 0;
            }

            KillDialogByHandleInternal(g_secondaryDialogHandle);
            return StartLoadDialog(0, character, dialogId, priority, loadInfo, 1,
                                   DialogStatus_PlayingPrimaryWithSecondary);
        }

        default:
            return 0;
    }
}

// PSX: rsDialogEvent (RSEVENT.CPP:82, 0x8003470C)
static s32 rsDialogEvent(s32 event, s32 param1, s32 param2, s32 param3) {
    MARKFUNCTION(0x8003470C);

    switch (event) {
        case RS_LOAD_DIALOG:
            return jcsLoadDialog(param1, param2, param3);

        case RS_PLAY_DIALOG:
        {
            LVector pos = {};
            const bool hasPos = ConsumePendingDialogPlayPosition(pos);
            const s32 playResult = PlayDialogRequest(param1, param2, hasPos ? &pos : nullptr, static_cast<u32>(param3));
            if (DialogTraceEnabled()) {
                LOG("[DialogTrace] RS_PLAY_DIALOG handle=%d timeout=%d returned=%d", param1, param3, playResult);
            }
            return playResult;
        }

        case RS_STOP_DIALOG:
        {
            return jcsStopDialog();
        }

        case RS_KILL_DIALOG:
            return KillDialogByHandleInternal(param1);

        case RS_LOAD_AND_PLAY_DIALOG:
        {
            const s32 handle = rsDialogEvent(RS_LOAD_DIALOG, param1, param2, 255);
            if (!handle) {
                return 0;
            }

            return rsDialogEvent(RS_PLAY_DIALOG, handle, param3, 360);
        }

        case RS_QUERY_DIALOG_PRIORITY:
            return jcsQueryDialogPriority();

        default:
            break;
    }

    return 0;
}

// PSX: gp+544 - global sound enabled flag (0 = enabled, nonzero = disabled)
static s32 g_soundDisabled = 0;

// PSX: gp+572 - pause flag
static s32 g_pauseFlag = 0;

// PSX: gp+732 - mute flag
static s32 g_muteFlag = 0;

// PSX: Sound location to music file mapping (gp-relative at 0x800ED6CC)
// Event 4 (RS_SET_LOCATION) with param1 sets the "current sound location"
// which determines which music track plays when RS_LEVEL_BEGIN fires.
s32 g_currentSoundLocation = 0;

bool g_deferLevelBeginMusic = false;

// PSX: gp[0x2E8] - current ambience space (set by jcsSetAmbienceSpace)
static s32 g_currentAmbienceSpace = 0;

// PSX music file table indexed by sound location (LocInfo at 0x800D6588)
// Matches PSX LocInfo[n][1] ordering exactly
static const char* s_musicTable[] = {
    "SOUND/MUSIC/CHINA1.FAG",    //  0: Chinatown petal 1
    "SOUND/MUSIC/CHINA2.FAG",    //  1: Chinatown petal 2
    "SOUND/MUSIC/CHINA3.FAG",    //  2: Chinatown petal 3
    "SOUND/MUSIC/CHINAB.FAG",    //  3: Chinatown boss
    "SOUND/MUSIC/WATER1.FAG",    //  4: Waterfront petal 1
    "SOUND/MUSIC/WATER2.FAG",    //  5: Waterfront petal 2
    "SOUND/MUSIC/WATER3.FAG",    //  6: Waterfront petal 3
    "SOUND/MUSIC/WATERB.FAG",    //  7: Waterfront boss
    "SOUND/MUSIC/SEWER1.FAG",    //  8: Sewer petal 1
    "SOUND/MUSIC/SEWER2.FAG",    //  9: Sewer petal 2
    "SOUND/MUSIC/SEWER3.FAG",    // 10: Sewer petal 3
    "SOUND/MUSIC/SEWERB.FAG",    // 11: Sewer boss
    "SOUND/MUSIC/ROOF1.FAG",     // 12: Rooftop petal 1
    "SOUND/MUSIC/ROOF2.FAG",     // 13: Rooftop petal 2
    "SOUND/MUSIC/ROOF3.FAG",     // 14: Rooftop petal 3
    "SOUND/MUSIC/ROOFB.FAG",     // 15: Rooftop boss
    "SOUND/MUSIC/FACTORY1.FAG",  // 16: Factory petal 1
    "SOUND/MUSIC/FACTORY2.FAG",  // 17: Factory petal 2
    "SOUND/MUSIC/FACTORY3.FAG",  // 18: Factory petal 3
    "SOUND/MUSIC/FACTORYB.FAG",  // 19: Factory boss
    "SOUND/MUSIC/TEMPLE.FAG",    // 20: Temple
    "SOUND/MUSIC/DESTSEL.FAG",   // 21: Destination select
    "SOUND/MUSIC/TITLE.FAG",     // 22: Title screen
    "SOUND/MUSIC/GAMEOVER.FAG",  // 23: Game over
    nullptr,                      // 24: movie (no FAG)
};
static constexpr s32 MUSIC_TABLE_COUNT = 25;

// PSX: per-location ambience file from LocInfo (@0x800D6588: flag word[2],
// inline name word[3]). Locations 0-21 all have an ambient bed; 22-24
// (title/gameover/movie) have none. (g_ambiance itself is defined earlier.)
static const char* s_ambienceTable[MUSIC_TABLE_COUNT] = {
    "SNDAMB1", // 0: Chinatown 1
    "SNDAMB1", // 1: Chinatown 2
    "SNDAMB1", // 2: Chinatown 3
    "SNDAMB1", // 3: Chinatown boss
    "SNDAMB2", // 4: Waterfront 1
    "SNDAMB2", // 5: Waterfront 2
    "SNDAMB2", // 6: Waterfront 3
    "SNDAMB2", // 7: Waterfront boss
    "SNDAMB3", // 8: Sewer 1
    "SNDAMB3", // 9: Sewer 2
    "SNDAMB3", // 10: Sewer 3
    "SNDAMB3", // 11: Sewer boss
    "SNDAMB4", // 12: Rooftop 1
    "SNDAMB4", // 13: Rooftop 2
    "SNDAMB4", // 14: Rooftop 3
    "SNDAMB4", // 15: Rooftop boss
    "SNDAMB5", // 16: Factory 1
    "SNDAMB5", // 17: Factory 2
    "SNDAMB5", // 18: Factory 3
    "SNDAMB5", // 19: Factory boss
    "SNDAMB6", // 20: Temple
    "SNDAMB7", // 21: Destination select
    nullptr,   // 22: Title
    nullptr,   // 23: Game over
    nullptr,   // 24: Movie
};

// PSX: CInteractiveMusicController::Think (MSCCTRLR.CPP:56, 0x80082960)
// Block-based FAG song switching. Decoded from PSX binary: location 9 (SEWER2)
// uses blocks 4-34 for song 1 (action), everything else song 0 (calm).
// Other locations with interactive music decoded similarly.
void InteractiveMusicControllerThink() {
    // NOTE: ambience is ticked by jcsUpdateAmbience() from the frame loops, not
    // here - this Think only runs during gameplay.
    if (!g_sound || !g_blockManager || !Player::s_player) return;
    if (g_deferLevelBeginMusic) return;

    const s32 loc = g_currentSoundLocation;
    if (loc < 0 || loc >= MUSIC_TABLE_COUNT) return;
    const char* path = s_musicTable[loc];
    if (!path) return;

    const s32 block = static_cast<s32>(
        g_blockManager->GetBlockNumber(Player::s_player->pos));
    if (block < 0 || block == 0x1000) return;

    // Determine desired song index from PSX MSCCTRLR.CPP block thresholds
    s32 desired = 0;
    if (loc == 9) {
        // SEWER2: blocks 4-34 → song 1 (train action), others → song 0
        desired = (block >= 4 && block < 35) ? 1 : 0;
    }
    else if (loc == 8) {
        // SEWER1: blocks 24+ → song 1
        desired = (block >= 24) ? 1 : 0;
    }
    else if (loc == 10) {
        // SEWER3: blocks 22+ → song 1
        desired = (block >= 22) ? 1 : 0;
    }
    else if (loc == 15) {
        // ROOFB: blocks 3+ → song 1
        desired = (block >= 3) ? 1 : 0;
    }
    else {
        return; // No interactive music for this location
    }

    static s32 s_lastBlock = -1;
    static s32 s_currentSong = 0;
    static s32 s_lastLocation = -1;

    if (loc != s_lastLocation) {
        s_lastBlock = -1;
        s_currentSong = 0;
        s_lastLocation = loc;
    }

    if (block == s_lastBlock) return;
    s_lastBlock = block;

    if (desired == s_currentSong) return;
    s_currentSong = desired;

    g_sound->PlayMusicTrackSong(path, static_cast<u32>(desired), g_sound->musicVolume);
}

// PSX: rsEvent (RSEVENT.CPP:48, 0x800346B0)
s32 rsEvent(s32 event, s32 param1, s32 param2, s32 param3) {
    MARKFUNCTION(0x800346B0);

    // PSX: reads gp+544 flag, returns 0 if sound disabled
    if (g_soundDisabled != 0) return 0;

    // PSX dispatch:
    // Events 26-31 -> rsDialogEvent
    // Events 1-23  -> jcsHandleControlEvent
    if (event >= 26 && event <= 31) {
        return rsDialogEvent(event, param1, param2, param3);
    }

    if (event >= 1 && event <= 23) {
        return jcsHandleControlEvent(event, param1, param2, param3);
    }

    return 0;
}

// PSX: jcsHandleControlEvent (JCSOUND.CPP:947, 0x80035B00)
s32 jcsHandleControlEvent(s32 event, s32 param1, s32 param2, s32 param3) {
    MARKFUNCTION(0x80035B00);

    if (!g_sound) return 0;

    switch (event) {
        case RS_INITIALIZE: // 1
            LOG("[rsEvent] Initialize");
            jcsInitializeDialog();
            break;

        case RS_TERMINATE: // 2
            LOG("[rsEvent] Terminate");
            jcsTerminateDialog();
            rsdWorld::StopAllPersistentSounds();
            g_ambiance.Close();
            g_sound->StopMusic();
            break;

        case RS_UNLOAD_LEVEL: // 3 - stop all sounds
            LOG("[rsEvent] UnloadLevel - stop all");
            rsdWorld::StopAllPersistentSounds();
            g_ambiance.Stop();
            g_sound->StopMusic();
            if (DialogTraceEnabled()) {
                LOG("[DialogTrace] UNLOAD pre-kill state=%d primary=%d secondary=%d started=%d lastChoice=%u",
                    g_dialogStatus, g_primaryDialogHandle, g_secondaryDialogHandle,
                    g_dialogSystemStarted, g_lastDialogChoiceOffset);
            }
            KillAllDialogHandles();
            if (DialogTraceEnabled()) {
                LOG("[DialogTrace] UNLOAD post-kill state=%d primary=%d secondary=%d started=%d",
                    g_dialogStatus, g_primaryDialogHandle, g_secondaryDialogHandle, g_dialogSystemStarted);
            }
            break;

        case RS_SET_LOCATION: // 4 - set sound location (music + SFX bank)
            LOG("[rsEvent] SetSoundLocation(%d)", param1);
            g_currentSoundLocation = param1;
            // PSX: LocInfo[loc][0] -> GAME.WAP location table -> bank index.
            // Mapping: location 0-20 -> bank 0-20, location 21+ -> bank 21.
            if (g_sound) {
                g_sound->activeSfxBank = (param1 < 21) ? param1 : 21;
#if defined(RC_PLATFORM_PSP)
                LOG("[SFX-DIAG] SetLocation param=%d activeBank=%d", param1, g_sound->activeSfxBank);
#endif
            }
            // PSX resets dialog header selection flags when switching level/location.
            if (EnsureDialogDataLoaded()) {
                ResetDialogHeaderState();
            }
            // PSX: jcsSetSoundLocation opens the per-location ambient bed
            // (rsdAmbiance::Open) when LocInfo[loc][2] is set.
            g_ambiance.Close();
            if (param1 >= 0 && param1 < MUSIC_TABLE_COUNT && s_ambienceTable[param1]) {
                g_ambiance.Open(s_ambienceTable[param1]);
            }
            break;

        case RS_LEVEL_BEGIN: // 5 - start music for current location
        {
            g_pauseFlag = 0;
            g_muteFlag = 0;
            // PSX: jcsStartSound starts the ambient bed alongside music. Use
            // FadeIn so it ramps up from silence on level entry (rsdAmbiance
            // starts, then the transition fades the engine in) rather than
            // popping in at full volume.
            g_ambiance.FadeIn(1500);
            if (g_deferLevelBeginMusic) {
                // Decode now, while still hidden behind the loading screen -
                // only the (cheap) actual playback start is deferred to the
                // fade-in via Sound::StartPreloadedMusic(), so the fade
                // doesn't stall on file I/O/decode.
                LOG("[rsEvent] LevelBegin - preloading for deferred start (location=%d)", g_currentSoundLocation);
                if (g_currentSoundLocation >= 0 && g_currentSoundLocation < MUSIC_TABLE_COUNT) {
                    const char* path = s_musicTable[g_currentSoundLocation];
                    if (path) {
                        g_sound->PreloadMusicTrack(path, g_sound->musicVolume);
                    }
                }
                break;
            }
            LOG("[rsEvent] LevelBegin - start music (location=%d, musicVol=%.2f)", g_currentSoundLocation, g_sound->musicVolume);
            if (g_currentSoundLocation >= 0 && g_currentSoundLocation < MUSIC_TABLE_COUNT) {
                const char* path = s_musicTable[g_currentSoundLocation];
                if (path) {
                    g_sound->PlayMusicTrack(path, g_sound->musicVolume);
                }
            }
            break;
        }

        case RS_STOP_MUSIC: // 6 - fade/stop music
            LOG("[rsEvent] StopMusic");
            g_pauseFlag = 0;
            jcsFadeOutEngine(14);
            break;

        case RS_FADE_OUT: // 7 - fade out
            LOG("[rsEvent] FadeOut(%d)", param1);
            jcsFadeOutEngine((u32)param1);
            break;

        case RS_FADE_OUT_2: // 8 - fade in
            LOG("[rsEvent] FadeIn(%d)", param1);
            if (!g_muteFlag) {
                jcsFadeInEngine((u32)param1);
            }
            break;

        case RS_PAUSE: // 9
            LOG("[rsEvent] Pause");
            g_pauseFlag = 1;
            break;

        case RS_MUTE: // 10
            LOG("[rsEvent] Mute");
            if (!g_muteFlag) {
                jcsFadeOutEngine(15);
                g_muteFlag = 1;
            }
            break;

        case RS_UNMUTE: // 11
            LOG("[rsEvent] Unmute");
            if (g_muteFlag) {
                if (!g_pauseFlag) {
                    jcsFadeInEngine(15);
                }
                g_muteFlag = 0;
            }
            break;

        case RS_CD_YIELD: // 12
            if ((param1 & 1) != 0) {
                jcsStopDialog();
            }
            break;

        case RS_CD_ACCESS: // 13
            if ((param1 & 1) != 0) {
                jcsStartDialog();
            }
            break;

        case RS_SET_MUSIC_VOL: // 16
        {
            // PSX: 80 * param / 100 -> SPU vol (0-100 range out of 127)
            f32 vol = (f32)param1 * 0.8f / 100.0f;
            LOG("[rsEvent] SetMusicVol(%d -> %.2f)", param1, vol);
            g_sound->SetMusicVolume(vol);
            break;
        }

        case RS_SET_EFFECTS_VOL_AUX: // 17
        {
            // PSX: no case handler - falls through to default (no-op)
            LOG("[rsEvent] SetEffectsVolAux(%d)", param1);
            break;
        }

        case RS_SET_EFFECTS_VOL: // 18
        {
            // PSX: stores raw value as effects vol
            f32 vol = (f32)param1 / 100.0f;
            LOG("[rsEvent] SetEffectsVol(%d -> %.2f)", param1, vol);
            g_sound->SetEffectsVolume(vol);
            break;
        }

        case RS_SET_DIALOG_VOL: // 19
        {
            // PSX: 83 * param / 100 -> SPU vol
            f32 vol = (f32)param1 * 0.83f / 100.0f;
            LOG("[rsEvent] SetDialogVol(%d -> %.2f)", param1, vol);
            g_sound->SetDialogVolume(vol);
            UpdateActiveDialogVoiceVolumes(vol);
            break;
        }

        case RS_SET_STEREO: // 14
            LOG("[rsEvent] SetStereo");
            AudioEngine::SetOutputMono(false);
            break;

        case RS_SET_MONO: // 15
            LOG("[rsEvent] SetMono");
            AudioEngine::SetOutputMono(true);
            break;

        case 20: // jcsSetAmbienceSpace + optional crossfade
            LOG("[rsEvent] SetAmbienceSpace(%d, crossfade=%d)", param1, param2);
            // PSX: jcsSetAmbienceCrossFade(dur) then jcsSetAmbienceSpace(space)
            // -> rsdAmbiance::SetCrossFadeDuration + SetSpace.
            g_currentAmbienceSpace = param1;
            if (param2 > 0) {
                g_ambiance.SetCrossFadeDuration(param2);
            }
            g_ambiance.SetSpace(param1);
            break;

        case 21: // jcsSetListener(playerPos, cameraData) - 3D audio listener
            rsdWorld::UpdateSpatialAudioState();
            break;

        case 22: // jcsFadeOutEngine(-1) - fade out all
            LOG("[rsEvent] FadeOutAll");
            jcsFadeOutEngine(0xFFFFFFFF);
            break;

        case 23: // jcsFadeInEngine(-1) - fade in all
            LOG("[rsEvent] FadeInAll");
            if (!g_muteFlag) {
                jcsFadeInEngine(0xFFFFFFFF);
            }
            break;

        default:
            LOG("[rsEvent] Unhandled control event %d (p1=%d, p2=%d, p3=%d)",
                event, param1, param2, param3);
            break;
    }

    return 1;
}

// PSX: jcsFadeInEngine__FUl (JCSOUND.CPP:764, 0x8003582C)
// Flags bitmask:
//   bit 1 (0x02): fade in music player
//   bit 2 (0x04): fade in ambiance
//   bit 0 (0x01): resume dialog
//   bit 3 (0x08): fade in persistent sounds + set reverb
void jcsFadeInEngine(u32 flags) {
    MARKFUNCTION(0x8003582C);

    if (!g_sound) return;

    if (flags & 0x02) {
        g_sound->UnmuteMusic();
    }

    // PSX: bit 2 -> rsdAmbiance::FadeIn (1500ms)
    if (flags & 0x04) {
        g_ambiance.FadeIn(1500);
    }

    if (flags & 0x01) {
        ResumeDialogTimeout(0);
        ResumeDialogTimeout(1);
    }

    // PSX: bit 3 -> rsdPersistent::FadeInAll(1500) + rsdSetReverb
    if (flags & 0x08) {
        rsdWorld::UnmuteAllPersistentSounds();
    }
}

// PSX: jcsFadeOutEngine__FUl (JCSOUND.CPP:721, 0x80035760)
// Flags bitmask:
//   bit 1 (0x02): fade out music player
//   bit 2 (0x04): fade out ambiance
//   bit 0 (0x01): pause dialog
//   bit 3 (0x08): fade out persistent sounds + save/clear reverb
// PC: music fade-out stops the music track.
void jcsFadeOutEngine(u32 flags) {
    MARKFUNCTION(0x80035760);

    if (!g_sound) return;

    if (flags & 0x02) {
        g_sound->MuteMusic();
    }

    // PSX: bit 2 -> rsdAmbiance::FadeOut(1500)
    if (flags & 0x04) {
        g_ambiance.FadeOut(1500);
    }

    if (flags & 0x01) {
        if (g_dialogStatus >= DialogStatus_PlayingPrimary
            && g_dialogStatus <= DialogStatus_PlayingPrimaryState14) {
            KillDialogByHandleInternal(g_primaryDialogHandle);
        }
        PauseDialogTimeout(0);
        PauseDialogTimeout(1);
    }

    // PSX: bit 3 -> rsdPersistent::FadeOutAll(1500) + save reverb + set reverb(0)
    if (flags & 0x08) {
        rsdWorld::MuteAllPersistentSounds();
    }
}

// PSX: jcsValidateHandle (JCSDLG.CPP:1639, 0x800434D8)
s32 jcsValidateHandle(s32 handle) {
    MARKFUNCTION(0x800434D8);

    if (!IsCurrentHandle(handle)) {
        return 0;
    }

    switch (g_dialogStatus) {
        case 1:
        case 2:
        case 5:
        case 8:
        case 10:
        case 13:
            return IsPrimaryHandle(handle);

        case 4:
            return IsSecondaryHandle(handle);

        case 6:
        case 7:
        case 9:
        case 11:
        case 12:
        case 14:
            return IsEitherHandle(handle);

        default:
            return 0;
    }
}

// PSX: jcsIsPlayable (JCSDLG.CPP:428, 0x80042908)
s32 jcsIsPlayable(s32 handle) {
    MARKFUNCTION(0x80042908);

    if (!jcsValidateHandle(handle)) {
        return 0;
    }

    switch (g_dialogStatus) {
        case 5:
        case 6:
        case 7:
        case 8:
            return IsPrimaryHandle(handle);

        case 9:
            if (IsSecondaryHandle(handle)) {
                return 1;
            }
            return IsPrimaryHandle(handle);

        default:
            return 0;
    }
}

// PSX: jcsIsPlaying__Fv (JCSDLG.CPP:488, 0x800429A4)
s32 jcsIsPlaying() {
    MARKFUNCTION(0x800429A4);
    return jcsIsPlaying(g_primaryDialogHandle);
}

// PSX: jcsIsPlaying(handle) (JCSDLG.CPP:506, 0x800429CC)
s32 jcsIsPlaying(s32 handle) {
    MARKFUNCTION(0x800429CC);

    s32 result = 0;
    if (jcsValidateHandle(handle) && g_dialogStatus >= 10 && g_dialogStatus < 15) {
        result = IsPrimaryHandle(handle);
    }

    return result;
}

// PSX: jcsQueryDialogPriority() (JCSDLG.CPP:1285, 0x80043218)
s32 jcsQueryDialogPriority() {
    MARKFUNCTION(0x80043218);

    if (g_primaryDialogHandle == 0) {
        return -1;
    }

    s32 bestPriority = g_primaryDialogPriority;
    if (bestPriority < g_secondaryDialogPriority) {
        bestPriority = g_secondaryDialogPriority;
    }

    return bestPriority;
}

// PSX: jcsQueryDialogPriority(handle) (JCSDLG.CPP:1312, 0x80043254)
s32 jcsQueryDialogPriority(s32 handle) {
    MARKFUNCTION(0x80043254);

    if (!jcsValidateHandle(handle)) {
        return -1;
    }

    if (IsPrimaryHandle(handle)) {
        return g_primaryDialogPriority;
    }

    return g_secondaryDialogPriority;
}

// PSX: KillAllDialog__Fv - kills secondary first, then primary.
static void KillAllDialogHandles() {
    const s32 secondaryHandle = g_secondaryDialogHandle;
    const s32 primaryHandle = g_primaryDialogHandle;
    KillDialogByHandleInternal(secondaryHandle);
    KillDialogByHandleInternal(primaryHandle);
}

// PSX: jcsStopDialog__Fv (JCSDLG.CPP:1586, 0x8004345C)
static s32 jcsStopDialog() {
    MARKFUNCTION(0x8004345C);

    KillAllDialogHandles();
    if (g_dialogReservedVoice != AUDIO_VOICE_INVALID) {
        AudioEngine::ReleaseVoice(g_dialogReservedVoice);
        g_dialogReservedVoice = AUDIO_VOICE_INVALID;
    }
    g_dialogSystemStarted = 0;
    return 0;
}

// PSX: jcsStartDialog__Fv (JCSDLG.CPP:1567, 0x8004344C)
void jcsStartDialog() {
    MARKFUNCTION(0x8004344C);

    if (g_dialogReservedVoice == AUDIO_VOICE_INVALID) {
        g_dialogReservedVoice = AudioEngine::ReserveVoice(AUDIO_PRIO_DIALOGUE);
    }
    g_dialogSystemStarted = 1;
}

AudioSample LoadDialogClipSample(s32 character, s32 dialogId, s32 variant) {
    const s32 savedForcedVariant = g_forcedDialogVariant;
    g_forcedDialogVariant = (variant >= 0) ? variant : kIsolatedRandomVariant;
    DialogClipSelection selection;
    const bool gotSelection = SelectDialogClip(character, dialogId, &selection);
    g_forcedDialogVariant = savedForcedVariant;

    if (!gotSelection) {
        return AUDIO_SAMPLE_INVALID;
    }

    // Deliberately never touches g_lastDialogChoiceOffset/g_dialogHeader's
    // used-bits, for either an exact or a randomized pick - this function is
    // menu-preview-only and must not perturb gameplay's own variety/anti-repeat
    // state (or vice versa: get rejected because gameplay happened to just
    // play the same clip).
    return LoadDialogSampleFromSelection(character, dialogId, selection);
}
