#include "gen/savegame.h"

#include "gen/ccfile.h"
#include "gen/game.h"
#include "gen/world.h"
#include "gen/scoremgr.h"
#include "ai/player.h"
#include "p3d/fileio.h"
#include "pc/apppaths.h"
#if NEW_CHEATS
#include "extra/cheats.h"
#endif
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <cstring>

static constexpr u32 kSaveMagic = 0x53415645u;    // SAVE
static constexpr u32 kSaveVersion = 1;
static constexpr const char* kSaveDir = apppaths::kUserFilesDir;
static constexpr const char* kSavePathFmt = apppaths::kSaveSlotFmt;
static constexpr const char* kAutosavePath = apppaths::kAutosavePath;

struct SaveGameBlob {
    u32 magic = kSaveMagic;
    u32 version = kSaveVersion;
    s64 savedUnixTime = 0;
    u32 levelIndex = 0;
    u32 petalIndex = 0;
    s32 livesLeft = Player::kMaxLives;
    PetalStats petalStats[21] = {};
    s32 drunkenMasterUnlocked = 0;
    s32 currentFightScore = 0;
    s32 currentComboScore = 0;
    s32 currentStyleScore = 0;
    u8 currentGrade = 0;
    u8 currentCollectCount = 0;
    u8 currentGoldDragons = 0;
    u8 reserved = 0;
};

static bool s_pendingLoadActive = false;
static u32 s_pendingLevelIndex = 0;
static u32 s_pendingPetalIndex = 0;
static bool s_pendingLivesActive = false;
static s32 s_pendingLivesLeft = Player::kMaxLives;

static bool BuildSlotPath(s32 slotIndex, char* outPath, s32 outPathLen) {
    if (!outPath || outPathLen <= 0) {
        return false;
    }

    if (slotIndex < 0 || slotIndex >= SAVEGAME_SLOT_COUNT) {
        return false;
    }

    snprintf(outPath, outPathLen, kSavePathFmt, static_cast<int>(slotIndex + 1));
    return true;
}

static void EnsureSaveDirectoryExists() {
    p3d::io::CreateDirectories(kSaveDir);
}

static bool ReadBlobAtPath(const char* path, SaveGameBlob* outBlob) {
    if (!outBlob) {
        return false;
    }

    if (!path || !path[0]) {
        return false;
    }

    ccFile file;
    if (!file.Open(path, ccFile::OPEN_READ)) {
        return false;
    }

    if (file.GetLength() < (s32)sizeof(SaveGameBlob)) {
        file.Close();
        return false;
    }

    SaveGameBlob blob = {};
    const s32 bytesRead = file.Read(&blob, (u32)sizeof(SaveGameBlob));
    file.Close();

    if (bytesRead != (s32)sizeof(SaveGameBlob)) {
        return false;
    }

    if (blob.magic != kSaveMagic || blob.version != kSaveVersion) {
        return false;
    }

    *outBlob = blob;
    return true;
}

static bool WriteBlobAtPath(const char* path, const SaveGameBlob& blobIn) {
    if (!path || !path[0]) {
        return false;
    }

    EnsureSaveDirectoryExists();

    ccFile file;
    if (!file.Open(path, ccFile::OPEN_WRITE)) {
        return false;
    }

    SaveGameBlob blob = blobIn;
    const s32 bytesWritten = file.Write(&blob, (u32)sizeof(SaveGameBlob));
    file.Close();
    return bytesWritten == (s32)sizeof(SaveGameBlob);
}

static bool ReadSlotBlob(s32 slotIndex, SaveGameBlob* outBlob) {
    char path[64] = {};
    return BuildSlotPath(slotIndex, path, (s32)sizeof(path)) && ReadBlobAtPath(path, outBlob);
}

static bool WriteSlotBlob(s32 slotIndex, const SaveGameBlob& blob) {
    char path[64] = {};
    return BuildSlotPath(slotIndex, path, (s32)sizeof(path)) && WriteBlobAtPath(path, blob);
}

static void FormatSaveDate(s64 unixTime, char* outText, s32 outTextLen) {
    if (!outText || outTextLen <= 0) {
        return;
    }

    outText[0] = '\0';
    if (unixTime <= 0) {
        return;
    }

    const time_t t = (time_t)unixTime;
    tm localTm = {};
#if defined(RC_PLATFORM_WINDOWS)
    if (localtime_s(&localTm, &t) != 0) {
        return;
    }
#else
    if (!localtime_r(&t, &localTm)) {
        return;
    }
#endif

    strftime(outText, (size_t)outTextLen, "%Y-%m-%d %H:%M", &localTm);
}

static bool BuildRuntimeBlob(SaveGameBlob* outBlob) {
    if (!outBlob || !g_game || !g_scoreManager) {
        return false;
    }

    World* world = g_game->GetWorld();
    if (!world) {
        return false;
    }

    SaveGameBlob blob = {};
    blob.magic = kSaveMagic;
    blob.version = kSaveVersion;
    blob.savedUnixTime = (s64)std::time(nullptr);

    u32 levelIndex = world->GetCurrentLevelIndex();
    u32 petalIndex = world->GetCurrentPetalIndex();
    if (levelIndex == 0xFFFFFFFFu) {
        levelIndex = world->GetTargetLevelIndex();
        petalIndex = world->GetTargetPetalIndex();
    }

    blob.levelIndex = levelIndex;
    blob.petalIndex = petalIndex;

    const s32 lives = Player::s_player ? Player::s_player->GetLivesLeft() : Player::kMaxLives;
    blob.livesLeft = lives;

    memcpy(blob.petalStats, g_scoreManager->petalStats, sizeof(blob.petalStats));
    blob.drunkenMasterUnlocked = g_scoreManager->drunkenMasterUnlocked;
    blob.currentFightScore = g_scoreManager->currentFightScore;
    blob.currentComboScore = g_scoreManager->currentComboScore;
    blob.currentStyleScore = g_scoreManager->currentStyleScore;
    blob.currentGrade = g_scoreManager->currentGrade;
    blob.currentCollectCount = g_scoreManager->currentCollectCount;
    blob.currentGoldDragons = g_scoreManager->currentGoldDragons;

    *outBlob = blob;
    return true;
}

static void ApplyLoadedScoreState(const SaveGameBlob& blob) {
    if (!g_scoreManager) {
        return;
    }

    memcpy(g_scoreManager->petalStats, blob.petalStats, sizeof(blob.petalStats));
    g_scoreManager->drunkenMasterUnlocked = blob.drunkenMasterUnlocked;
    g_scoreManager->currentFightScore = blob.currentFightScore;
    g_scoreManager->currentComboScore = blob.currentComboScore;
    g_scoreManager->currentStyleScore = blob.currentStyleScore;
    g_scoreManager->currentGrade = blob.currentGrade;
    g_scoreManager->currentCollectCount = blob.currentCollectCount;
    g_scoreManager->currentGoldDragons = blob.currentGoldDragons;

    g_scoreManager->checkpointFightScore = g_scoreManager->currentFightScore;
    g_scoreManager->checkpointComboScore = g_scoreManager->currentComboScore;
    g_scoreManager->checkpointStyleScore = g_scoreManager->currentStyleScore;

    g_scoreManager->checkpointGradeCollect = 0;
    u8* packed = reinterpret_cast<u8*>(&g_scoreManager->checkpointGradeCollect);
    packed[0] = g_scoreManager->currentGrade;
    packed[1] = g_scoreManager->currentCollectCount;
    packed[2] = g_scoreManager->currentGoldDragons;
}

static bool PopulateSlotInfo(const SaveGameBlob& blob, SaveGameSlotInfo* outInfo) {
    if (!outInfo) {
        return false;
    }

    outInfo->occupied = true;
    FormatSaveDate(blob.savedUnixTime, outInfo->dateText, (s32)sizeof(outInfo->dateText));
    outInfo->livesLeft = blob.livesLeft;
    u32 totalRedDragons = 0;
    u32 totalGoldDragons = 0;
    for (const PetalStats& stats : blob.petalStats) {
        totalRedDragons += stats.collectCount;
        totalGoldDragons += stats.goldDragons;
    }
    outInfo->redDragons = (u8)(totalRedDragons > 255 ? 255 : totalRedDragons);
    outInfo->goldDragons = (u8)(totalGoldDragons > 255 ? 255 : totalGoldDragons);
    // The serialized load target is commonly the destination-select hub. For
    // menu metadata, report the furthest regular zone progression has unlocked
    // instead: -2 is locked, while -1 or greater is playable/completed.
    u32 playableLevelIndex = 0;
    for (u32 level = 0; level < 5; ++level) {
        if (blob.petalStats[level * 3].fightScore >= -1) {
            playableLevelIndex = level;
        }
    }
    outInfo->nextLevelIndex = playableLevelIndex;
    outInfo->nextPetalIndex = 0;
    return true;
}

bool SaveGameQuerySlotInfo(s32 slotIndex, SaveGameSlotInfo* outInfo) {
    if (!outInfo) {
        return false;
    }

    *outInfo = {};

    SaveGameBlob blob = {};
    if (!ReadSlotBlob(slotIndex, &blob)) {
        return false;
    }

    return PopulateSlotInfo(blob, outInfo);
}

bool SaveGameQueryAutosaveInfo(SaveGameSlotInfo* outInfo) {
    if (!outInfo) {
        return false;
    }

    *outInfo = {};
    SaveGameBlob blob = {};
    return ReadBlobAtPath(kAutosavePath, &blob) && PopulateSlotInfo(blob, outInfo);
}

bool SaveGameWriteSlot(s32 slotIndex) {
    SaveGameBlob blob = {};
    if (!BuildRuntimeBlob(&blob)) {
        return false;
    }

    return WriteSlotBlob(slotIndex, blob);
}

bool SaveGameHasAutosave() {
    SaveGameBlob blob = {};
    return ReadBlobAtPath(kAutosavePath, &blob);
}

bool SaveGameWriteAutosave() {
    SaveGameBlob blob = {};
    return BuildRuntimeBlob(&blob) && WriteBlobAtPath(kAutosavePath, blob);
}

bool SaveGameDeleteAutosave() {
    const int result = std::remove(kAutosavePath);
    return result == 0 || errno == ENOENT;
}

bool SaveGameDeleteSlot(s32 slotIndex) {
    char path[64] = {};
    if (!BuildSlotPath(slotIndex, path, (s32)sizeof(path))) {
        return false;
    }

    const int result = std::remove(path);
    return result == 0 || errno == ENOENT;
}

bool SaveGameLoadSlot(s32 slotIndex) {
    SaveGameBlob blob = {};
    if (!ReadSlotBlob(slotIndex, &blob)) {
        return false;
    }

#if NEW_CHEATS
    ResetCheats();
#endif
    ApplyLoadedScoreState(blob);

    s_pendingLevelIndex = blob.levelIndex;
    s_pendingPetalIndex = blob.petalIndex;
    s_pendingLivesLeft = blob.livesLeft;
    s_pendingLoadActive = true;
    s_pendingLivesActive = true;

    if (Player::s_player) {
        Player::s_player->SetLivesLeft(s_pendingLivesLeft);
    }

    return true;
}

bool SaveGameLoadAutosave() {
    SaveGameBlob blob = {};
    if (!ReadBlobAtPath(kAutosavePath, &blob)) {
        return false;
    }

#if NEW_CHEATS
    ResetCheats();
#endif
    ApplyLoadedScoreState(blob);
    s_pendingLevelIndex = blob.levelIndex;
    s_pendingPetalIndex = blob.petalIndex;
    s_pendingLivesLeft = blob.livesLeft;
    s_pendingLoadActive = true;
    s_pendingLivesActive = true;
    if (Player::s_player) {
        Player::s_player->SetLivesLeft(s_pendingLivesLeft);
    }
    return true;
}

bool SaveGameHasPendingLoad() {
    return s_pendingLoadActive;
}

bool SaveGameApplyPendingLoad(Game* game) {
    if (!s_pendingLoadActive || !game) {
        return false;
    }

    World* world = game->GetWorld();
    if (!world) {
        return false;
    }

    world->SetTargetLevelPetal(s_pendingLevelIndex, s_pendingPetalIndex);
    world->ResetLevel();
    s_pendingLoadActive = false;

    if (Player::s_player) {
        Player::s_player->SetLivesLeft(s_pendingLivesLeft);
    }

    return true;
}

void SaveGameApplyPendingLives() {
    if (!s_pendingLivesActive || !Player::s_player) {
        return;
    }

    Player::s_player->SetLivesLeft(s_pendingLivesLeft);
    s_pendingLivesActive = false;
}
