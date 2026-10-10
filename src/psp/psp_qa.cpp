#include "psp/psp_qa.h"

#if defined(RC_PLATFORM_PSP) && defined(PSP_AUTOPLAY)

#include "ai/door.h"
#include "ai/player.h"
#include "ai/thing.h"
#include "gen/ai.h"
#include "gen/game.h"
#include "gen/scoremgr.h"
#include "gen/world.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kQaLogPath = "AUTOPLAY_LOG.TXT";
constexpr size_t kQaBufferSize = 8 * 1024;
constexpr size_t kQaLineMax = 256;
constexpr u32 kQaSnapshotPeriod = 300;
constexpr int kQaMaxDoors = 64;

char g_qaBuffer[kQaBufferSize];
size_t g_qaUsed = 0;
u32 g_qaTick = 0;

struct QaDoorEntry {
    const Door* door;
    u16 uniqueID;
    s32 state;
    u32 seenTick;
};

QaDoorEntry g_qaDoors[kQaMaxDoors];
int g_qaDoorCount = 0;

void QaWriteOut() {
    if (g_qaUsed == 0) return;
    if (std::FILE* file = std::fopen(kQaLogPath, "a")) {
        std::fwrite(g_qaBuffer, 1, g_qaUsed, file);
        std::fclose(file);
    }
    // Drop the batch even if the file could not be opened; the buffer must
    // never block gameplay.
    g_qaUsed = 0;
}

World* QaWorld() {
    return g_game ? g_game->GetWorld() : nullptr;
}

void QaTrackDoors() {
    if (!g_ai) return;

    for (ccMinNode* node = g_ai->moveList.head; node; node = node->next) {
        Thing* thing = static_cast<Thing*>(node);
        if (thing->thingType != AITypes::TT_DOOR) continue;
        const Door* door = static_cast<const Door*>(thing);

        int index = -1;
        for (int i = 0; i < g_qaDoorCount; i++) {
            if (g_qaDoors[i].door == door && g_qaDoors[i].uniqueID == door->uniqueID) {
                index = i;
                break;
            }
        }

        if (index < 0) {
            if (g_qaDoorCount >= kQaMaxDoors) continue;
            index = g_qaDoorCount++;
            g_qaDoors[index] = { door, door->uniqueID, door->doorState, g_qaTick };
            QaLog("door id=%u name=%s state=new->%ld pos=%ld,%ld,%ld",
                  static_cast<unsigned>(door->uniqueID), QaName(door->name),
                  static_cast<long>(door->doorState), static_cast<long>(door->pos.x),
                  static_cast<long>(door->pos.y), static_cast<long>(door->pos.z));
            continue;
        }

        QaDoorEntry& entry = g_qaDoors[index];
        entry.seenTick = g_qaTick;
        if (entry.state != door->doorState) {
            QaLog("door id=%u name=%s state=%ld->%ld open=%ld/%ld",
                  static_cast<unsigned>(door->uniqueID), QaName(door->name),
                  static_cast<long>(entry.state), static_cast<long>(door->doorState),
                  static_cast<long>(door->currentOpen), static_cast<long>(door->maxOpenDist));
            entry.state = door->doorState;
        }
    }

    // Forget doors that left moveList (petal unload/reload) so a reused address
    // is reported as a new door instead of a bogus transition.
    int kept = 0;
    for (int i = 0; i < g_qaDoorCount; i++) {
        if (g_qaDoors[i].seenTick == g_qaTick) g_qaDoors[kept++] = g_qaDoors[i];
    }
    g_qaDoorCount = kept;
}

void QaSnapshot() {
    const Player* player = Player::s_player;
    const World* world = QaWorld();
    const ScoreManager* score = g_scoreManager;
    QaLog("snap pos=%ld,%ld,%ld hp=%u/%u lives=%ld red=%u gold=%u cp=%d cpblock=%ld "
          "level=%ld petal=%ld levelID=%ld",
          player ? static_cast<long>(player->pos.x) : 0L,
          player ? static_cast<long>(player->pos.y) : 0L,
          player ? static_cast<long>(player->pos.z) : 0L,
          player ? static_cast<unsigned>(player->health) : 0u,
          player ? static_cast<unsigned>(player->maxHealth) : 0u,
          player ? static_cast<long>(player->livesLeft) : -1L,
          score ? static_cast<unsigned>(score->currentCollectCount) : 0u,
          score ? static_cast<unsigned>(score->currentGoldDragons) : 0u,
          (player && player->checkpoint.IsValid()) ? 1 : 0,
          player ? static_cast<long>(player->checkpoint.field24) : -1L,
          world ? static_cast<long>(world->GetCurrentLevelIndex()) : -1L,
          world ? static_cast<long>(world->GetCurrentPetalIndex()) : -1L,
          world ? static_cast<long>(world->GetCurLevelID()) : -1L);
}

} // namespace

const char* QaName(const char* name) {
    return (name && name[0]) ? name : "-";
}

void QaLog(const char* fmt, ...) {
    char line[kQaLineMax];
    int prefix = std::snprintf(line, sizeof(line), "QA t=%lu ", static_cast<unsigned long>(g_qaTick));
    if (prefix < 0) return;
    if (static_cast<size_t>(prefix) >= sizeof(line)) prefix = static_cast<int>(sizeof(line) - 1);

    va_list args;
    va_start(args, fmt);
    int body = std::vsnprintf(line + prefix, sizeof(line) - static_cast<size_t>(prefix), fmt, args);
    va_end(args);
    if (body < 0) return;

    size_t length = static_cast<size_t>(prefix) + static_cast<size_t>(body);
    if (length > sizeof(line) - 2) length = sizeof(line) - 2;  // truncated line
    line[length++] = '\n';

    if (g_qaUsed + length > kQaBufferSize) QaWriteOut();
    std::memcpy(g_qaBuffer + g_qaUsed, line, length);
    g_qaUsed += length;
}

void QaFlush() {
    QaWriteOut();
}

void QaAutoplayTick() {
    g_qaTick++;
    QaTrackDoors();
    if (g_qaTick % kQaSnapshotPeriod == 0) {
        QaSnapshot();
        // Periodic flush so a crash loses at most ~300 ticks of probes.
        QaFlush();
    }
}

u32 QaForcedButtons(u32 frame) {
    // Scripted extra buttons for pspGamepad::Poll, as {firstFrame, lastFrame, mask}.
    // Empty by default (mask 0); never include SELECT, START+SELECT closes the game.
    struct Window { u32 first, last, mask; };
    static constexpr Window kWindows[] = { { 0u, 0u, 0u } };
    u32 mask = 0;
    for (const Window& w : kWindows) {
        if (frame >= w.first && frame <= w.last) mask |= w.mask;
    }
    return mask;
}

void QaLogWorld(const char* tag) {
    const World* world = QaWorld();
    const Player* player = Player::s_player;
    QaLog("%s level=%ld petal=%ld target=%ld/%ld levelID=%ld cp=%d red=%u gold=%u",
          QaName(tag),
          world ? static_cast<long>(world->GetCurrentLevelIndex()) : -1L,
          world ? static_cast<long>(world->GetCurrentPetalIndex()) : -1L,
          world ? static_cast<long>(world->GetTargetLevelIndex()) : -1L,
          world ? static_cast<long>(world->GetTargetPetalIndex()) : -1L,
          world ? static_cast<long>(world->GetCurLevelID()) : -1L,
          (player && player->checkpoint.IsValid()) ? 1 : 0,
          g_scoreManager ? static_cast<unsigned>(g_scoreManager->currentCollectCount) : 0u,
          g_scoreManager ? static_cast<unsigned>(g_scoreManager->currentGoldDragons) : 0u);
    QaFlush();
}

#endif
