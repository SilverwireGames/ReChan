#include "pc/debugui.h"

#if defined(PSP_AUTO_START_PHASE1)
#include "gen/camera.h"
#include "gen/common.h"
#include "gen/game.h"
#include "gen/world.h"
#include "p3d/context.h"
#include "p3d/input.h"
#include "pc/inputaction.h"
#include "psp/psp_perf.h"
#include "gen/time.h"
#include "p3d/p3dmath.h"
#include "pddi/psp/psprender.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>

#include "psp/psp_funcprof.h"

#if defined(PSP_BENCH_CROWD)
#include "ai/humanoid.h"
#include "ai/player.h"
#include "gen/ai.h"
#endif

namespace {

#if defined(PSP_BENCH_COMBAT)
// Combat benchmark: the normal follow camera and scripted fighting input
// (pspGamepad::Poll); a longer warm-up lets nearby enemies engage first.
constexpr u32 kWarmupFrames = 300;
#else
constexpr u32 kWarmupFrames = 180;
#endif
constexpr u32 kMeasurementFrames = 600;
// Ten 60-frame segments make one complete route fit exactly inside the
// 600-frame measurement window.
constexpr u32 kSegmentFrames = 60;

struct BenchmarkScenario {
    s32 levelId;
    const char* shortName;
};

// Keep the benchmark underhood-only and cover representative gameplay maps.
// Level 7 is the hub/destination area used by the manual PSP baseline.
static constexpr BenchmarkScenario kScenarios[] = {
    { 1, "CHINATOWN" },
    { 2, "WATERFRONT" },
    { 3, "SEWER" },
    { 4, "ROOF" },
    { 5, "FACTORY" },
    { 6, "TEMPLE" },
    { 7, "HUB" },
};

struct RoutePoint {
    s32 eyeX;
    s32 eyeY;
    s32 eyeZ;
    s32 targetX;
    s32 targetY;
    s32 targetZ;
};

// Keep the camera close to the loaded checkpoint in every map. A large
// world-space route was valid for phase 1 but could leave the resident block
// set in another scenario, making the renderer walk geometry that was never
// loaded. This local orbit still changes the visible material/depth mix while
// keeping the benchmark deterministic and inside the active block.
static constexpr RoutePoint kRoute[] = {
    {    0,   0,    0,    0,   0,    0 },
    {  192,   0,    0,  128,   0,    0 },
    {  192,   0,  192,  128,   0,  128 },
    {    0,   0,  256,    0,   0,  160 },
    { -192,   0,  192, -128,   0,  128 },
    { -192,   0,    0, -128,   0,    0 },
    { -192,   0, -192, -128,   0, -128 },
    {    0,   0, -256,    0,   0, -160 },
    {  192,   0, -192,  128,   0, -128 },
    {  192,   0,    0,  128,   0,    0 },
    {    0,   0,    0,    0,   0,    0 },
};

static bool s_baseCaptured = false;
static bool s_measurementStarted = false;
static bool s_scenarioFinished = false;
static bool s_allScenariosFinished = false;
static bool s_transitionQueued = false;
// Phase 1.1 is an interactive capture: the player walks through the first
// door, stops in the two-enemy room, then arms the underhood benchmark with
// L+R+START. This avoids guessing a checkpoint coordinate and guarantees the
// measured population is the real room the QA pass is investigating.
static bool s_phase11Active = false;
static u32 s_scenarioIndex = 0;
static s32 s_activeLevelId = -1;
static u32 s_routeFrame = 0;
static u32 s_measurementFrames = 0;
static LVector s_baseEye = {};
static LVector s_baseTarget = {};

#if defined(PSP_BENCH_CROWD)
// Crowd scenario: early in each scenario, move up to five of the level's
// regular enemies (no bosses) into a ring around Jackie, so the measured
// window renders and runs up to six fighting characters. Humanoid damage is
// off in combat benchmarks, so the fight lasts the whole window.
constexpr u32 kCrowdGatherFrame = 60;
constexpr u32 kCrowdMax = 5;

static bool IsCrowdCandidate(const Humanoid* humanoid) {
    const u16 type = humanoid->thingType;
    if (type < AITypes::TT_HUMANOID_FIRST || type > 28u) return false;
    switch (type) {
        case AITypes::TT_GRONTAR:
        case AITypes::TT_PAUL:
        case AITypes::TT_OSCAR:
        case AITypes::TT_DANTE:
        case AITypes::TT_BUTCH:
            return false;
        default:
            return humanoid->health != 0;
    }
}

static void GatherCrowd() {
    Player* player = Player::s_player;
    if (!player || !g_ai) return;
    static constexpr s32 kRing[kCrowdMax][2] = {
        { 350, 0 }, { 108, 333 }, { -283, 206 }, { -283, -206 }, { 108, -333 },
    };
    u32 moved = 0;
    for (ccMinNode* node = g_ai->humanoidList.head; node && moved < kCrowdMax; node = node->next) {
        Humanoid* humanoid = static_cast<Humanoid*>(static_cast<Thing*>(node));
        if (humanoid == player || !IsCrowdCandidate(humanoid)) continue;
        LVector target = player->pos;
        target.x += kRing[moved][0];
        target.z += kRing[moved][1];
        humanoid->Teleport(target);
        // Jackie's block is active: create the model now, as block activation
        // would, instead of drawing the no-model debug box.
        humanoid->Activate();
        ++moved;
    }
    PspPerf::SetScenarioNote(moved);
}
#endif

static s32 Lerp(s32 from, s32 to, u32 numerator, u32 denominator) {
    return from + static_cast<s32>((static_cast<s64>(to - from) * numerator) / denominator);
}

} // namespace
#endif

namespace DebugUI {

void Init() {}

// Benchmark output is intentionally underhood-only. Drawing a large text HUD
// changes the GU workload and makes the result less useful for optimization.
void Draw() {}

#if defined(RC_PLATFORM_PSP)
void BeginBenchmarkFrame(Game* game) {
#if defined(PSP_AUTO_START_PHASE1) && defined(PSP_BENCH_FIXED_STEP)
    // First Play frame of each scenario, before its logic runs: restart the
    // random sequences, the 30 Hz tick phase and the scripted input, so the
    // scenario no longer depends on what ran (or how long loading took)
    // before it.
    static s32 seededLevelId = -1;
    if (!game || game->GetState() != GameState::Play || !game->GetWorld()) return;
    const s32 levelId = game->GetWorld()->GetCurLevelID();
    if (levelId == seededLevelId) return;
    seededLevelId = levelId;
    rmRandomSeedRef() = 0x12345678u;
    std::srand(1u);
    if (g_time) g_time->ResetPlayPresentationState();
#if defined(PSP_BENCH_COMBAT)
    ResetPspBenchCombatInput();
#endif
#else
    (void)game;
#endif
}

void UpdateBenchmarkCamera(Camera* camera) {
#if defined(PSP_AUTO_START_PHASE1)
    if (!camera) return;

    if (!g_game || !g_game->GetWorld()) return;
    World* world = g_game->GetWorld();
    const s32 levelId = world->GetCurLevelID();
    if (levelId != s_activeLevelId) {
        for (u32 i = 0; i < static_cast<u32>(sizeof(kScenarios) / sizeof(kScenarios[0])); ++i) {
            if (kScenarios[i].levelId == levelId) {
                s_scenarioIndex = i;
                break;
            }
        }
        s_activeLevelId = levelId;
        s_baseCaptured = false;
        s_measurementStarted = false;
        s_scenarioFinished = false;
        s_transitionQueued = false;
        s_phase11Active = false;
        s_routeFrame = 0;
        s_measurementFrames = 0;
        PspPerf::WriteState("play", levelId,
            static_cast<s32>(world->GetCurrentLevelIndex()),
            static_cast<s32>(world->GetTargetLevelIndex()));
    }

#if defined(PSP_BENCH_PHASE11_ONLY)
    if (levelId != 1 || !p3d::input) {
        camera->ClearBenchmarkMode();
        return;
    }
    if (!s_phase11Active) {
        camera->ClearBenchmarkMode();
        const bool arm = p3d::input->IsGamepadButtonDown(GpBtn::LB)
            && p3d::input->IsGamepadButtonDown(GpBtn::RB)
            && p3d::input->IsGamepadButtonTriggered(GpBtn::Start);
        if (!arm) return;

        // Capture the exact post-door camera/player location supplied by the
        // real PSP run. The first 180 frames are warm-up; only the following
        // 600 frames are written to PSP_PERF_PHASE1_1.TXT.
        s_phase11Active = true;
        s_baseCaptured = false;
        s_measurementStarted = false;
        s_scenarioFinished = false;
        s_routeFrame = 0;
        s_measurementFrames = 0;
        LOG("[PSP Bench] phase 1.1 armed at first-door room");
    }
#endif

    if (s_allScenariosFinished || s_scenarioFinished) {
        camera->ClearBenchmarkMode();
        return;
    }

    if (!s_baseCaptured) {
        s_baseEye = camera->GetPosition();
        s_baseTarget = camera->GetTargetPos();
        // A freshly loaded Play camera can still have a zero target before
        // its first follow-path update. Aim at the scene ahead in that case.
        if (s_baseTarget.x == 0 && s_baseTarget.y == 0 && s_baseTarget.z == 0) {
            s_baseTarget = { s_baseEye.x, s_baseEye.y, s_baseEye.z + 1600 };
        }
        s_baseCaptured = true;
    }

#if defined(PSP_BENCH_CROWD)
    if (s_routeFrame == kCrowdGatherFrame) {
        GatherCrowd();
    }
#endif
    if (!s_measurementStarted && s_routeFrame == kWarmupFrames) {
        PspPerf::BeginMeasurementWindow();
        // Profile exactly the measured frames (no-op unless PSP_FUNC_PROFILE).
        PspFuncProfile::Start();
        s_measurementStarted = true;
        s_measurementFrames = 0;
    }

    constexpr u32 kPointCount = static_cast<u32>(sizeof(kRoute) / sizeof(kRoute[0]));
    constexpr u32 kRouteFrames = (kPointCount - 1u) * kSegmentFrames;
    const u32 routeFrame = kRouteFrames == 0 ? 0 : s_routeFrame % kRouteFrames;
    const u32 point = routeFrame / kSegmentFrames;
    const u32 segmentFrame = routeFrame % kSegmentFrames;
    const RoutePoint& from = kRoute[point];
    const RoutePoint& to = kRoute[point + 1u];

    LVector eye = {
        s_baseEye.x + Lerp(from.eyeX, to.eyeX, segmentFrame, kSegmentFrames),
        s_baseEye.y + Lerp(from.eyeY, to.eyeY, segmentFrame, kSegmentFrames),
        s_baseEye.z + Lerp(from.eyeZ, to.eyeZ, segmentFrame, kSegmentFrames),
    };
    LVector target = {
        s_baseTarget.x + Lerp(from.targetX, to.targetX, segmentFrame, kSegmentFrames),
        s_baseTarget.y + Lerp(from.targetY, to.targetY, segmentFrame, kSegmentFrames),
        s_baseTarget.z + Lerp(from.targetZ, to.targetZ, segmentFrame, kSegmentFrames),
    };
#if defined(PSP_BENCH_COMBAT)
    (void)eye;
    (void)target;
    camera->ClearBenchmarkMode();
#else
    camera->SetBenchmarkPose(eye, target);
#endif
    ++s_routeFrame;
#else
    (void)camera;
#endif
}

void OnBenchmarkFrameRecorded() {
#if defined(PSP_AUTO_START_PHASE1) && defined(PSP_PERF_TELEMETRY)
    if (!s_measurementStarted || s_scenarioFinished) return;
    ++s_measurementFrames;
    if (s_measurementFrames >= kMeasurementFrames) {
        char path[48] = {};
#if defined(PSP_BENCH_PHASE11_ONLY)
        std::snprintf(path, sizeof(path), "PSP_PERF_PHASE1_1.TXT");
#else
        std::snprintf(path, sizeof(path), "PSP_PERF_S%02lu_L%02ld.TXT",
            static_cast<unsigned long>(s_scenarioIndex + 1u),
            static_cast<long>(kScenarios[s_scenarioIndex].levelId));
#endif
#if defined(PSP_FUNC_PROFILE)
        {
            char profilePath[24] = {};
            std::snprintf(profilePath, sizeof(profilePath), "PROF_S%02lu.TXT",
                static_cast<unsigned long>(s_scenarioIndex + 1u));
            PspFuncProfile::StopAndWrite(profilePath);
        }
#endif
        PspPerf::WriteReportTo(path, s_scenarioIndex + 1u, s_activeLevelId);
#if defined(PSP_BENCH_FIXED_STEP)
        {
            // The same simulated frame on every run: a pixel diff between two
            // builds shows any rendering change.
            char shotPath[24] = {};
            std::snprintf(shotPath, sizeof(shotPath), "SHOT_S%02lu.BMP",
                static_cast<unsigned long>(s_scenarioIndex + 1u));
            WritePspLastFrame(shotPath);
        }
#endif
        LOG("[PSP Bench] scenario %lu complete: levelId=%ld frames=%lu report=%s",
            static_cast<unsigned long>(s_scenarioIndex + 1u),
            static_cast<long>(kScenarios[s_scenarioIndex].levelId),
            static_cast<unsigned long>(s_measurementFrames),
            path);
        s_scenarioFinished = true;
#if defined(PSP_BENCH_PHASE11_ONLY)
        s_phase11Active = false;
#endif
        s_allScenariosFinished = s_scenarioIndex + 1u >=
            static_cast<u32>(sizeof(kScenarios) / sizeof(kScenarios[0]));
    }
#endif
}

bool IsBenchmarkFinished() {
#if defined(PSP_AUTO_START_PHASE1)
    return s_scenarioFinished;
#else
    return false;
#endif
}

bool QueueNextBenchmarkScenario(Game* game) {
#if defined(PSP_AUTO_START_PHASE1)
#if defined(PSP_BENCH_PHASE11_ONLY)
    (void)game;
    return false;
#else
    if (!game || !s_scenarioFinished || s_allScenariosFinished || s_transitionQueued) {
        return false;
    }

    World* world = game->GetWorld();
    if (!world) return false;

    const u32 nextScenario = s_scenarioIndex + 1u;
    const s32 nextLevelIndex = world->LevelIDToIndex(kScenarios[nextScenario].levelId);
    world->SetTargetLevelPetal(static_cast<u32>(nextLevelIndex), 0);
    world->ResetLevel();
    g_selectedLevel = static_cast<s16>(nextLevelIndex);
    s_transitionQueued = true;
    PspPerf::WriteState("queue", kScenarios[nextScenario].levelId,
        nextLevelIndex, static_cast<s32>(world->GetTargetLevelIndex()));
    LOG("[PSP Bench] queue scenario %lu: levelId=%ld index=%ld",
        static_cast<unsigned long>(nextScenario + 1u),
        static_cast<long>(kScenarios[nextScenario].levelId),
        static_cast<long>(nextLevelIndex));
    game->SetState(GameState::QueueLevelLoad);
    return true;
#endif
#else
    (void)game;
    return false;
#endif
}
#endif

bool IsEnabled() { return false; }
bool IsHumanoidDamageDisabled() {
#if defined(PSP_BENCH_COMBAT)
    // Nobody dies, so every scenario window stays a fight.
    return true;
#else
    return false;
#endif
}
bool ShouldBlockGameInput() {
#if defined(PSP_BENCH_PHASE11_ONLY)
    return s_phase11Active;
#else
    return false;
#endif
}
bool IsPlayerInputAllowed() { return true; }
bool IsDebugCameraInputAllowed() { return false; }
}
