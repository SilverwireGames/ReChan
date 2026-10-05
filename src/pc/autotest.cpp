#include "pc/autotest.h"
#include "ai/generator.h"
#include "ai/player.h"
#include "gen/ai.h"
#include "gen/blockmgr.h"
#include "gen/control.h"
#include "gen/world.h"
#include "gen/game.h"
#include "gen/time.h"
#include "p3d/context.h"
#include "pddi/pddidev.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vendor/stb/stb_image_write.h"

namespace {
    bool s_enabled = false;
    s32 s_levelIndex = 0;
    s32 s_levelId = -1;
    s32 s_petalIndex = 0;
    s32 s_warpGenerator = -1;
    bool s_traceSpawns = false;
    s32 s_killInterval = 0;
    s32 s_frames = 300;
    s32 s_fps = 60;
    u32 s_heldButtons = 0;
    std::string s_screenshotPath;

    s32 s_playFrames = 0;
    bool s_captureRequested = false;
    bool s_captureDone = false;
    bool s_finished = false;
    int s_exitCode = 0;
    f64 s_startTime = 0.0;

    // Generous: a cold level load on a slow disk is well under this.
    constexpr f64 kReachGameplayTimeoutSec = 180.0;

    struct ButtonName {
        const char* name;
        u32 mask;
    };

    const ButtonName kButtonNames[] = {
        { "up", PsxPad::Up },
        { "down", PsxPad::Down },
        { "left", PsxPad::Left },
        { "right", PsxPad::Right },
        { "cross", PsxPad::Cross },
        { "circle", PsxPad::Circle },
        { "square", PsxPad::Square },
        { "triangle", PsxPad::Triangle },
        { "l1", PsxPad::L1 },
        { "r1", PsxPad::R1 },
        { "l2", PsxPad::L2 },
        { "r2", PsxPad::R2 },
    };

    u32 ParseButtons(const char* list) {
        u32 mask = 0;
        std::string token;
        for (const char* c = list;; ++c) {
            if (*c == ',' || *c == '\0') {
                for (const ButtonName& b : kButtonNames) {
                    if (token == b.name) {
                        mask |= b.mask;
                    }
                }
                token.clear();
                if (*c == '\0') {
                    break;
                }
            }
            else {
                token += *c;
            }
        }
        return mask;
    }

    // Puts the player on the Nth EnemyGenerator so its room's spawn logic runs.
    void WarpToGenerator(s32 wanted) {
        Player* player = Player::s_player;
        if (!g_ai || !player) {
            return;
        }

        s32 index = 0;
        for (ccMinNode* n = g_ai->moveList.head; n; n = n->next) {
            Thing* thing = static_cast<Thing*>(n);
            if (thing->thingType != AITypes::TT_ENEMYGENERATOR) {
                continue;
            }
            if (index++ != wanted) {
                continue;
            }

            player->pos = thing->pos;
            player->homePos = thing->pos;
            player->UpdatePosition();
            if (g_blockManager) {
                const u16 block = g_blockManager->GetBlockNumber(thing->pos);
                player->blockNum = (block != 4096) ? block : thing->blockNum;
            }
            LOG_ALWAYS("[AutoTest] Warped to generator %d at (%d,%d,%d) block=%u", wanted,
                       thing->pos.x, thing->pos.y, thing->pos.z, (u32)player->blockNum);
            return;
        }
        LOG_ALWAYS("[AutoTest] Generator %d not found (%d in level)", wanted, index);
    }

    void CaptureOverlay() {
        if (!s_captureRequested || s_captureDone) {
            return;
        }
        s_captureDone = true;

        std::vector<unsigned char> rgb;
        int width = 0;
        int height = 0;
        if (!p3d::display->ReadBackBuffer(rgb, width, height)
            || !stbi_write_png(s_screenshotPath.c_str(), width, height, 3, rgb.data(), width * 3)) {
            AutoTest::Fail(12, "screenshot failed");
            return;
        }
        LOG_ALWAYS("[AutoTest] Wrote %s (%dx%d)", s_screenshotPath.c_str(), width, height);
    }
}

void AutoTest::ParseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const bool hasValue = (i + 1 < argc);
        if (std::strcmp(argv[i], "--autotest") == 0 && hasValue) {
            s_enabled = true;
            s_levelIndex = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--level-id") == 0 && hasValue) {
            s_levelId = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--warp-generator") == 0 && hasValue) {
            s_warpGenerator = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--kill-interval") == 0 && hasValue) {
            s_killInterval = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--trace-spawns") == 0) {
            s_traceSpawns = true;
        }
        else if (std::strcmp(argv[i], "--petal") == 0 && hasValue) {
            s_petalIndex = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--frames") == 0 && hasValue) {
            s_frames = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--fps") == 0 && hasValue) {
            s_fps = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--hold") == 0 && hasValue) {
            s_heldButtons = ParseButtons(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--screenshot") == 0 && hasValue) {
            s_screenshotPath = argv[++i];
        }
    }

    if (s_fps < 1) {
        s_fps = 60;
    }

    if (s_enabled) {
        LOG_ALWAYS("[AutoTest] level=%d petal=%d frames=%d fps=%d hold=0x%04X screenshot=%s",
                   s_levelIndex, s_petalIndex, s_frames, s_fps, s_heldButtons,
                   s_screenshotPath.empty() ? "none" : s_screenshotPath.c_str());
    }
}

bool AutoTest::IsEnabled() {
    return s_enabled;
}

s32 AutoTest::GetLevelIndex() {
    if (s_levelId >= 0 && g_game && g_game->GetWorld()) {
        const s32 index = g_game->GetWorld()->LevelIDToIndex(s_levelId);
        if (index >= 0) {
            return index;
        }
    }
    return s_levelIndex;
}

bool AutoTest::TraceSpawns() {
    return s_enabled && s_traceSpawns;
}

s32 AutoTest::GetPetalIndex() {
    return s_petalIndex;
}

s32 AutoTest::GetFps() {
    return s_fps;
}

f32 AutoTest::GetFixedDeltaTime() {
    return 1.0f / (f32)s_fps;
}

u32 AutoTest::GetHeldButtons() {
    return (s_enabled && s_playFrames > 0) ? s_heldButtons : 0;
}

void AutoTest::Fail(int exitCode, const char* reason) {
    LOG_ALWAYS("[AutoTest] FAILED: %s", reason);
    s_exitCode = exitCode;
    s_finished = true;
}

bool AutoTest::Step(GameState state) {
    if (!s_enabled) {
        return true;
    }
    if (s_finished) {
        return false;
    }

    if (s_startTime == 0.0) {
        s_startTime = Time::GetTimeInSeconds();
        p3d::display->AddOverlayCallback(CaptureOverlay);
    }

    if (state != GameState::Play) {
        if (s_playFrames == 0 && Time::GetTimeInSeconds() - s_startTime > kReachGameplayTimeoutSec) {
            Fail(11, "never reached gameplay");
            return false;
        }
        return true;
    }

    ++s_playFrames;
    if (s_playFrames == 30 && s_warpGenerator >= 0) {
        WarpToGenerator(s_warpGenerator);
    }
    if (s_killInterval > 0 && s_playFrames > 30 && (s_playFrames % s_killInterval) == 0 && g_ai) {
        // Stand-in for the player winning fights: removes one active enemy.
        for (ccMinNode* n = g_ai->humanoidList.head; n; n = n->next) {
            Thing* thing = static_cast<Thing*>(n);
            if (thing != Player::s_player && (thing->flags & TF_ACTIVATED) && !(thing->flags & TF_DEAD)) {
                LOG_ALWAYS("[AutoTest] Killing enemy %p", (void*)thing);
                thing->flags |= TF_DEAD;
                break;
            }
        }
    }
    if (s_playFrames < s_frames) {
        return true;
    }

    if (!s_screenshotPath.empty() && !s_captureDone) {
        s_captureRequested = true;
        return true;
    }

    LOG_ALWAYS("[AutoTest] Finished after %d gameplay frames", s_playFrames);
    s_finished = true;
    return false;
}

int AutoTest::GetExitCode() {
    return s_exitCode;
}
