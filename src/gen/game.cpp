// game.cpp
#include "common.h"
#include "gen/game.h"
#include "gen/display.h"
#include "gen/envmgr.h"
#include "gen/camera.h"
#include "gen/backg.h"
#include "gen/world.h"
#include "gen/block.h"
#include "gen/charmgr.h"
#include "gen/database.h"
#include "gen/cammgr.h"
#include "gen/levelmgr.h"
#include "gen/time.h"
#include "gen/ai.h"
#include "gen/model.h"
#include "gen/director.h"
#include "gen/effects.h"
#include "gen/scoremgr.h"
#include "gen/savegame.h"
#include "gen/control.h"
#include "snd/sound.h"
#include "snd/rsevent.h"
#include "snd/fesnd.h"
#include "ai/player.h"
#include "ai/fevolume.h"
#include "fe/femenumgr.h"
#include "fe/gamemenu.h"
#include "fe/titlescreen.h"
#include "fe/gameoverscreen.h"
#include "fe/xcfont.h"
#include "gen/animmgr.h"
#include "gen/animstruct.h"
#include "gen/animmat.h"
#include "fe/hud.h"
#include "radmovie/movieplayer.h"
#include "pc/debugui.h"
#include "pc/inputaction.h"
#include "pc/tim.h"
#include "p3d/input.h"
#include "p3d/context.h"
#include "fe/loadanim.h"
#include "config.h"
#include "p3d/input.h"
#include "p3d/texture.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include "pc/settings.h"
#include "radlib/rtask.h"
#include "pc/textmgr.h"
#include "extra/version.h"

#if !defined(RC_PLATFORM_PSP)
#include <thread>
#endif

#if CUSTOM_TEXT
#include "extra/customtext.h"
#endif

#if CUSTOM_MENU
#include "extra/fecustommenumgr.h"
#endif

#if AUTO_UPDATER
#include "extra/autoupdater.h"
#endif

#include "extra/shadowcsm.h"

#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
#include "psp/psp_perf.h"
#endif

// Global game pointer
Game* g_game = nullptr;

// PSX globals
s16 g_selectedLevel = -1;   // gp+44: queued level ID (-1 = none)
s32 g_directorActive = 0;   // gp+20: directorTimeOut gate used by gsPlayState
s32 g_feInitialized = 0;    // gp+88: FE memory puddle initialized

bool g_drawDebugConsole = false;

const Game::StateFunc Game::sStateTable[static_cast<int>(GameState::COUNT)] = {
    gsNullState,
    gsIntroState,
    gsTitleState,
    gsTitleLoopState,
    gsInitState,
    gsOpenFEState,
    gsFEState,
    gsPrePlayState,
    gsPlayState,
    gsEndLevelState,
    gsEndLevelLoopState,
    gsEndLevelExitState,
    gsPlayMovieCredits,
    gsDbgMenuState,
    gsMenuState,
    gsErrorState,
    gsErrorLoopState,
    gsErrorExitState,
    gsLocationMenuState,
    gsOpenLocationState,
    gsQueueLevelLoad,
    gsQueuePetalLoad,
    gsQueueLevelPetalLoad,
    gsDetermineNextGameState,
    gsDetermineGameOverState,
    gsEndGameState,
    gsEndGameLoopState,
    gsEndState,
};

Game::Game() {
    MARKFUNCTION(0x800C9AEC); // __4Game

    controlVal[0] = 0;
    controlVal[1] = 0;
    field136 = 0;

    // PSX: Game constructor creates 4 handlers in handlerSet2
    // BeginFrameHandler (pri=64), DrawEverythingHandler (pri=-16),
    // AnimateEverythingHandler (pri=-48), EndFrameHandler (pri=-64)
    // Display::InternalOpen adds dispBeginFrameHandler (62) and dispEndFrameHandler (-62)
    handlerSet2.AddHandler(BeginFrameHandler, 64);
    handlerSet2.AddHandler(AnimateEverythingHandler, -48);
    handlerSet2.AddHandler(DrawEverythingHandlerCB, -16);
    handlerSet2.AddHandler(EndFrameHandler, -64);
#if CUSTOM_MENU
    // PC: runs after world/HUD/director-overlay draws (-16/-31/-40) but before
    // dispEndFrameHandler (-62) swaps the frame, so the overlay is visible.
    handlerSet2.AddHandler(PlayFadeInHandlerCB, -55);
#endif

    SetState(GameState::Null);
    g_game = this;
    LOG("[Game] Created");
}

Game::~Game() {
    // Clean up intro/title resources
    if (introTexture) {
        introTexture->Release();
        introTexture = nullptr;
    }
    if (titleScreen) {
        delete titleScreen;
        titleScreen = nullptr;
    }
    if (gameOverScreen) {
        delete gameOverScreen;
        gameOverScreen = nullptr;
    }
    FreeXconFE();
    if (g_oxFontFile) {
        delete g_oxFontFile;
        g_oxFontFile = nullptr;
    }
    ScreenDraw::Shutdown();

    // Close and delete all managers
    while (ccMinNode* n = managerList.RemHead()) {
        Manager* mgr = static_cast<Manager*>(n);
        mgr->Close();
        delete mgr;
    }

    Close();
    g_game = nullptr;
}

// PSX: InternalOpen__4Game (GAME.CPP:2888, 0x800C9D08)
// Creates all game managers and adds them to managerList, then calls Open() on each.
void Game::InternalOpen() {
    MARKFUNCTION(0x800C9D08);

#if CUSTOM_TEXT
    // PC: Initialize custom text system (new localization system)
    g_customText.Init();
#endif

    // PSX creates managers in this order:
    // 1. tCellAlligator (8204) - memory allocator (not needed on PC)
    // 2. oxScreenManager (48) + FontInit - screen/font (handled in FE init paths)

    // 3. Time (PSX: 40 bytes)
    g_time = new Time();
    g_time->SetName("Time", 0);
    managerList.AddNodePri(g_time);

    // 4. AI (116) - AI master
    g_ai = new AI();
    g_ai->SetName("AI", 0);
    managerList.AddNodePri(g_ai);

    // 5. World (PSX: 160 bytes)
    World* world = new World();
    world->SetName("World", 0);
    managerList.AddNodePri(world);
    worldManager = world;

    // 6. EnvironmentManager (140) - environment effects
    EnvironmentManager* environmentMgr = new EnvironmentManager();
    environmentMgr->SetName("EnvironmentManager", 0);
    managerList.AddNodePri(environmentMgr);

    // 7. Display (32) - owns tView, BeginFrame/EndFrame, frame counter
    Display* disp = new Display();
    disp->SetName("Display", 0);
    managerList.AddNodePri(disp);
    // PSX: Display::InternalOpen registers dispBeginFrameHandler (pri=62)
    // and dispEndFrameHandler (pri=-62) into handlerSet2.
    handlerSet2.AddHandler(Display::dispBeginFrameHandler, 62);
    handlerSet2.AddHandler(Display::dispEndFrameHandler, -62);

    // 8. Director (212) - scripting/cutscenes
    g_director = new Director();
    g_director->SetName("Director", 0);
    managerList.AddNodePri(g_director);

    // 9. InputManager (PSX: 1492 bytes)
    if (!g_inputManager) {
        g_inputManager = new InputManager();
        g_inputManager->SetName("InputManager", 0);
        managerList.AddNodePri(g_inputManager);
    }

    // 10. LevelManager (PSX: 136 bytes)
    LevelManager* levelMgr = new LevelManager();
    levelMgr->SetName("LevelManager", 0);
    managerList.AddNodePri(levelMgr);

    // 11. Database (PSX: 120 bytes)
    Database* database = new Database();
    database->SetName("Database", 0);
    managerList.AddNodePri(database);

    // 12. Sound (PSX: 44 bytes)
    g_sound = new Sound();
    g_sound->SetName("Sound", 0);
    managerList.AddNodePri(g_sound);

    // 13. CameraManager (PSX: 60 bytes)
    CameraManager* camMgr = new CameraManager();
    camMgr->SetName("CameraManager", 0);
    managerList.AddNodePri(camMgr);

    // 14. BlockManager (PSX: 168 bytes)
    g_blockManager = new BlockManager();
    g_blockManager->SetName("BlockManager", 0);
    managerList.AddNodePri(g_blockManager);

    // 15. AnimationManager (40)
    AnimationManager* animMgr = new AnimationManager();
    animMgr->SetName("AnimationManager", 0);
    managerList.AddNodePri(animMgr);

    // 16. CharacterManager (PSX: 3004 bytes)
    g_characterManager = new CharacterManager();
    g_characterManager->SetName("CharacterManager", 0);
    managerList.AddNodePri(g_characterManager);

    // 17. ScoreManager (504) - score/collectibles
    g_scoreManager = new ScoreManager();
    g_scoreManager->SetName("ScoreManager", 0);
    managerList.AddNodePri(g_scoreManager);

    // Open all managers in list
    for (ccMinNode* n = managerList.head; n; n = n->next) {
        Manager* mgr = static_cast<Manager*>(n);
        mgr->Open();
    }

    g_settings.Load(SETTINGS_PATH);

    LOG("[Game] InternalOpen: managers created");
}

// PSX: InternalClose__4Game (GAME.CPP:3003)
void Game::InternalClose() {
    MARKFUNCTION(0x8002A184);

#if CUSTOM_TEXT
    // Shutdown custom text system
    g_customText.Shutdown();
#endif

    // Destroy player entity
    if (Player::s_player) {
        delete Player::s_player;
    }

    // Close all managers in reverse order
    for (ccMinNode* n = managerList.tail; n; ) {
        ccMinNode* prev = n->prev;
        Manager* mgr = static_cast<Manager*>(n);
        mgr->Close();
        n = prev;
    }

    g_blockManager = nullptr;
    worldManager = nullptr;
}

// PSX: InternalReset__4Game
void Game::InternalReset() {
    for (ccMinNode* n = managerList.head; n; n = n->next) {
        Manager* mgr = static_cast<Manager*>(n);
        mgr->Reset();
    }
}

// PSX: ProcessHandlers__4Game (GAME.CPP:2756, 0x8002B4F0)
// Iterates both handler sets and calls each handler's funcPtr
static void ProcessHandlerList(ccMinList& list, const char* pspProbeLabel = nullptr) {
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    static u32 pspHandlerProbeCalls = 0;
    const bool probePspHandlers = pspProbeLabel != nullptr && pspHandlerProbeCalls < 2;
    if (probePspHandlers) {
        LOG("[PSP Handler] %s list=%p head=%p tail=%p",
            pspProbeLabel, static_cast<void*>(&list),
            static_cast<void*>(list.head), static_cast<void*>(list.tail));
    }
#else
    (void)pspProbeLabel;
#endif
    u32 handlerIndex = 0;
    for (ccMinNode* n = list.head; n; ) {
        ccMinNode* next = n->next;
        Handler* h = static_cast<Handler*>(n);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspHandlers) {
            LOG("[PSP Handler] %s index=%lu handler=%p func=%p begin",
                pspProbeLabel,
                static_cast<unsigned long>(handlerIndex),
                static_cast<void*>(h),
                reinterpret_cast<void*>(h->funcPtr));
        }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        if (handlerIndex < 16) {
            char trace[48];
            std::snprintf(trace, sizeof(trace), "render: handler %lu begin",
                          static_cast<unsigned long>(handlerIndex));
            PspPerf::Trace(trace);
        }
#endif
        if (h->funcPtr) {
            h->funcPtr(h);
        }
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspHandlers) {
            LOG("[PSP Handler] %s index=%lu handler=%p done",
                pspProbeLabel,
                static_cast<unsigned long>(handlerIndex),
                static_cast<void*>(h));
        }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        if (handlerIndex < 16) {
            char trace[48];
            std::snprintf(trace, sizeof(trace), "render: handler %lu done",
                          static_cast<unsigned long>(handlerIndex));
            PspPerf::Trace(trace);
        }
#endif
        n = next;
        ++handlerIndex;
    }
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    if (probePspHandlers) {
        ++pspHandlerProbeCalls;
    }
#endif
}

static void UpdateSpatialAudioListener() {
    // PSX updates the 3D audio listener from player/camera every gameplay frame.
    // PC derives the listener from the current camera inside the sound backend.
    rsEvent(21, 0, 0, 0);
}

void Game::ProcessHandlers() {
    MARKFUNCTION(0x8002B4F0);

    // Process handlerSet1 (think/logic handlers)
    ProcessHandlerList(handlerSet1.handlerList);

    // Process handlerSet2 (draw/render handlers)
    ProcessHandlerList(handlerSet2.handlerList);
}

Camera& Game::GetCamera() {
    return *g_display->GetCamera();
}

tView& Game::GetView() {
    return g_display->GetView();
}

// World manager accessor
World* Game::GetWorld() const {
    return worldManager;
}

// Handler callbacks
// PSX: BeginFrameHandler (GAME.CPP:2192, pri=64 in handlerSet2)
// Resets render counter. Frame begin/end is handled by Display handlers.
void Game::BeginFrameHandler(Handler*) {
    MARKFUNCTION(0x8002B408);
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: game begin");
#endif
    // PSX: gp[3404] = 0 (reset render counter)
}

// PSX: AnimateEverythingHandler (GAME.CPP:2620, pri=-48)
static void AnimateLoop(ccList& list) {
    for (ccMinNode* node = list.head; node != nullptr; node = node->next) {
        Thing* thing = static_cast<Thing*>(node);
        if (thing->model) {
            Model* m = static_cast<Model*>(thing->model);
            m->Animate();
        }
    }
}

#if HIGH_FPS_PLAY_PRESENTATION
static void CaptureHumanoidAttackJointsLoop(ccList& list) {
    for (ccMinNode* node = list.head; node != nullptr; node = node->next) {
        Thing* thing = static_cast<Thing*>(node);
        HumanoidModel* hm = thing->model ? dynamic_cast<HumanoidModel*>(static_cast<Model*>(thing->model)) : nullptr;
        if (hm && hm->animMatrices) {
            hm->animMatrices->Swap();
            hm->CaptureAttackJointMatrices();
        }
    }
}

static void AdvanceHumanoidRenderInterpolationLoop(ccList& list) {
    for (ccMinNode* node = list.head; node != nullptr; node = node->next) {
        Thing* thing = static_cast<Thing*>(node);
        Humanoid* humanoid = static_cast<Humanoid*>(thing);
        humanoid->AdvanceRenderInterpolationTick();
    }
}
#endif

static void SyncAnimationTicksLoop(ccList& list, s32 tick) {
    for (ccMinNode* node = list.head; node != nullptr; node = node->next) {
        Thing* thing = static_cast<Thing*>(node);
        if (!thing->model) {
            continue;
        }

        Model* model = static_cast<Model*>(thing->model);
        AnimStructure* anim = static_cast<AnimStructure*>(model->animStructure);
        if (!anim) {
            continue;
        }

        anim->prevTick = tick;
        anim->currentTick = tick;
    }
}

// Pause/menu renders do not execute gameplay animation handlers, so keep
// AnimStructure tick baselines in sync to avoid large post-resume deltas.
static void SyncPausedAnimationTicks() {
    if (!g_ai || !g_time) {
        return;
    }

    const s32 tick = static_cast<s32>(g_time->frameCounter);
    SyncAnimationTicksLoop(g_ai->humanoidList, tick);
    SyncAnimationTicksLoop(g_ai->pickupList, tick);
    SyncAnimationTicksLoop(g_ai->inactivePickupList, tick);
    SyncAnimationTicksLoop(g_ai->moveList, tick);
}

// PSX: animLoopDSTACK__Fv (GAME.CPP:2634, 0x8002B368)
// Updates camera anim, animates active AI model lists, and advances effects.
static void animLoopDSTACK() {
    MARKFUNCTION(0x8002B368);

    if (g_display && g_display->GetCamera()) {
        g_display->GetCamera()->UpdateAnim();
    }

    if (g_ai) {
#if HIGH_FPS_PLAY_PRESENTATION
        CaptureHumanoidAttackJointsLoop(g_ai->humanoidList);
#endif
        AnimateLoop(g_ai->humanoidList);
        AnimateLoop(g_ai->pickupList);
        AnimateLoop(g_ai->inactivePickupList);
        AnimateLoop(g_ai->moveList);
#if HIGH_FPS_PLAY_PRESENTATION
        AdvanceHumanoidRenderInterpolationLoop(g_ai->humanoidList);
#endif
    }

    Effects_UpdateAll();
}

static void AdvanceGameplayAnimationOneTick() {
    animLoopDSTACK();
}

void Game::AnimateEverythingHandler(Handler*) {
    MARKFUNCTION(0x8002B2F0);
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: animate begin");
#endif
#if HIGH_FPS_PLAY_PRESENTATION
    if (g_game && g_game->GetState() == GameState::Play) {
        return;
    }
#endif
    animLoopDSTACK();
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: animate done");
#endif
}

// PSX: DrawEverythingHandler__FP7Handler (GAME.CPP:2211, 0x8002A98C)
// 1532 bytes, 91 blocks. Sorts blocks by distance, renders world geometry,
// characters, items, shadows per-block. Uses tView layers for ordering.
// NOTE: tView::BeginRender/EndRender are handled by Display's handlers (pri 62/-62).
void Game::DrawEverythingHandlerCB(Handler*) {
    MARKFUNCTION(0x8002A98C);
#if defined(PSP_STAGE_DIAGNOSTIC)
    static u32 pspWorldStageProbe = 0;
#endif
#if defined(RC_PLATFORM_PSP) && (!defined(NDEBUG) || defined(PSP_STAGE_DIAGNOSTIC))
    static u32 pspWorldCallbackProbe = 0;
    if (pspWorldCallbackProbe < 4u) {
        Log::Get().LogMessage("[PSP World] callback entry #%lu", static_cast<unsigned long>(pspWorldCallbackProbe++));
    }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: world begin");
#endif
    if (!g_game) return;
    World* world = g_game->GetWorld();
    if (!world) return;

    // PSX: sorts blocks by distance from camera, then iterates:
    //   EnterLayer(view, 0) -> DrawBG -> ExitLayer
    //   per-block: Render geometry, LookAt camera, render items/characters/shadows

    p3d::context->EnableZBuffer(true);
    p3d::context->SetBlendMode(PDDI_BLEND_NONE);
    p3d::context->SetCullMode(PDDI_CULL_NONE);

#if defined(RC_PLATFORM_PSP)
#if defined(PSP_STAGE_DIAGNOSTIC)
    if (pspWorldStageProbe < 4u) {
        Log::Get().LogMessage("[PSP Stage] VRAM flipbook bridge bypassed");
    }
#endif
    // The legacy face flipbook path mutates streamed texture data through a
    // desktop-oriented callback. On PSP it can receive a stale puppet after
    // the first frame and block the render thread before the next display
    // list. Keep the native frame path deterministic; static face textures
    // remain available and the bridge can be reintroduced once it is backed
    // by a validated PSP upload queue.
#else
    if (g_director) {
#if HIGH_FPS_PLAY_PRESENTATION
        const bool isPlayRenderOnlyFrame = (g_game && g_game->GetState() == GameState::Play && g_time && !g_time->DidPlayLogicStepThisFrame());
        if (!isPlayRenderOnlyFrame) {
#endif
        g_director->updateVramAnims();
#if HIGH_FPS_PLAY_PRESENTATION
        }
#endif
    }
#endif

    // PSX: DrawBG__5BackG is called before block/world geometry draw.
#if defined(PSP_STAGE_DIAGNOSTIC)
    if (pspWorldStageProbe < 4u) Log::Get().LogMessage("[PSP Stage] before background #%lu", static_cast<unsigned long>(pspWorldStageProbe));
#endif
    BackG::DrawBG();
#if defined(PSP_STAGE_DIAGNOSTIC)
    if (pspWorldStageProbe < 4u) Log::Get().LogMessage("[PSP Stage] after background #%lu", static_cast<unsigned long>(pspWorldStageProbe));
#endif

#if MODERN_GRAPHICS
    // Prepare this frame's shadow cascades. World::Render runs a caster-only
    // prepass before drawing block geometry receivers. No-op when Shadow
    // Quality is Low.
    ShadowCSM::BeginFrame();
#endif
#if defined(PSP_STAGE_DIAGNOSTIC)
    if (pspWorldStageProbe < 4u) Log::Get().LogMessage("[PSP Stage] before player/render #%lu", static_cast<unsigned long>(pspWorldStageProbe));
#endif

    // PSX: passes player position (MEMORY[0x1C] = thePlayer->pos) to DrawEverythingHandler,
    // NOT the camera position. Used for block distance sorting and seam offsets.
    const LVector& playerPos = Player::s_player ? Player::s_player->pos
        : g_display->GetCamera()->GetPosition();
#if defined(PSP_STAGE_DIAGNOSTIC)
    static u32 pspWorldCallbackStageProbe = 0;
    if (pspWorldCallbackStageProbe < 4u) {
        Log::Get().LogMessage("[PSP Stage] world before Render #%lu", static_cast<unsigned long>(pspWorldCallbackStageProbe));
    }
#endif
    world->Render(&playerPos);
#if defined(PSP_STAGE_DIAGNOSTIC)
    if (pspWorldCallbackStageProbe < 4u) {
        Log::Get().LogMessage("[PSP Stage] world after Render #%lu", static_cast<unsigned long>(pspWorldCallbackStageProbe++));
    }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: world done");
#endif

    // PSX: entities are drawn inside DrawEverythingHandler with VRAM active,
    // per-block via DrawLoop (humanoidList, inactivePickupList, pickupList, moveList).
    // Player is in humanoidList and drawn there. Nothing extra needed here.
}

// PSX: MenuRender__FP7MenuMgr (GAME.CPP:1714, 0x80029D68)
// Renders the game world behind a menu overlay, then the menu itself.
static void MenuRender(MenuMgr* menuMgr) {
    MARKFUNCTION(0x80029D68);
    // PSX: DrawEverythingHandler(null)
    Game::DrawEverythingHandlerCB(nullptr);
    // PSX: DrawDirectorOverlays(null)
    DrawDirectorOverlays(nullptr);
    // PSX: HUD::Display()
    if (g_hud) {
        g_hud->Display();
    }
    // PSX: if menuMgr: menuMgr->Render() (via oxScreenManager)
    if (menuMgr) {
        menuMgr->Render();
    }
}

// PSX: MenuDraw__FP7MenuMgr (GAME.CPP:1735, 0x80029DB8)
// Calls Invoke on the menu, renders game + menu, returns Invoke result.
static s32 MenuDraw(MenuMgr* menuMgr) {
    MARKFUNCTION(0x80029DB8);
    s32 result;
    if (menuMgr) {
        result = menuMgr->Invoke();
    }
    else {
        result = 1;
    }
    // PSX: Display::BeginFrame, MenuRender, Display::EndFrame
    MenuMgr* renderMgr = menuMgr;
    if (result == 8 || result == 4) {
        renderMgr = nullptr;
    }
    g_display->BeginFrame();
    MenuRender(renderMgr);
    g_display->EndFrame();
    return result;
}

static bool PumpMenuFadeFrame(f64 frameStart) {
    if (p3d::display) {
        p3d::display->PollEvents();
        if (p3d::display->ShouldClose()) {
            return false;
        }
    }
    if (p3d::input) {
        p3d::input->ServiceInput();
    }
    if (g_actionInput) {
        g_actionInput->Update(p3d::input);
    }
    if (g_time) {
        g_time->Step();
    }
    ++rFrameCount;
    ++rFrameCount60;
    rDoTaskList(&rMainTaskList, static_cast<u32>(rFrameCount60));
    // Keep ambience fades advancing during the blocking menu-fade / loading
    // sub-loop (the main loop is stalled here, so its tick doesn't run).
    jcsUpdateAmbience();
    jcsUpdateDialogCD();
    if (g_time) {
        g_time->WaitForFrameEnd(frameStart);
    }
    return true;
}

// PSX: MenuFade__Fv (GAME.CPP:1757, 0x80029E34)
// Blocks until dialog playback gate clears, then runs a blocking fade loop.
void Game::MenuFade() {
    MARKFUNCTION(0x80029E34);
    LOG("[Game] MenuFade: begin");

    // Safety bound: if the dialogue gate never clears (e.g. a stuck load
    // state), don't hang the game forever waiting for it - proceed with
    // the fade anyway after a couple of seconds.
    constexpr f64 kMaxDialogWaitSec = 2.0;
    const f64 dialogWaitStart = Time::GetTimeInSeconds();
    while (jcsIsPlaying()) {
        if (Time::GetTimeInSeconds() - dialogWaitStart > kMaxDialogWaitSec) {
            LOG("[Game] MenuFade: jcsIsPlaying() stuck past %.1fs, proceeding with fade anyway", kMaxDialogWaitSec);
            break;
        }
        const f64 frameStart = Time::GetTimeInSeconds();
        if (!PumpMenuFadeFrame(frameStart)) {
            return;
        }
    }
    LOG("[Game] MenuFade: dialog gate complete");

    FadeBegin();
    s32 fadeFrames = 0;
    while (FadeUpdate()) {
        ++fadeFrames;
        if ((fadeFrames % 30) == 0) {
            LOG("[Game] MenuFade: frame=%d", fadeFrames);
        }
        const f64 frameStart = Time::GetTimeInSeconds();
        g_display->BeginFrame();
#if defined(RC_PLATFORM_PSP)
        // The PSP loading fade must not redraw the pre-level world. That
        // render path can traverse stale FE/world handlers while the level is
        // being torn down and is unnecessary for a black loading transition.
#else
        MenuRender(nullptr);
#endif
        FadeRender();
        g_display->EndFrame();
        if (!PumpMenuFadeFrame(frameStart)) {
            return;
        }
    }
    FadeEnd();
    LOG("[Game] MenuFade: complete (%d frames)", fadeFrames);
}

#if CUSTOM_MENU
void Game::RenderTitleWithCustomBackground(bool drawPressStartOverlay) {
    if (!titleScreen) {
        return;
    }

    if (!g_feCustomMenuMgr || !g_feCustomMenuMgr->DrawTitleScreen()) {
        return;
    }

    if (drawPressStartOverlay) {
        s32 promptX = (s32)DEFAULT_SCREEN_WIDTH / 2;
        s32 promptY = 192;
        if (titleScreen->pressStartText) {
            promptX = titleScreen->pressStartText->mtx.GetX();
            promptY = titleScreen->pressStartText->mtx.GetY();
        }

        g_feCustomMenuMgr->DrawTitleStartPrompt(promptX, promptY);
    }
}

void Game::RenderGameOverWithCustomBackground() {
    if (!gameOverScreen) {
        return;
    }

    if (!g_feCustomMenuMgr || !g_feCustomMenuMgr->DrawGameOverScreen()) {
        return;
    }

    if (gameOverScreen->continueText) {
        g_feCustomMenuMgr->DrawGameOverContinuePrompt(gameOverScreen->continueText->mtx.GetX(),
                                                      gameOverScreen->continueText->mtx.GetY(),
                                                      gameOverScreen->continueText->colorR,
                                                      gameOverScreen->continueText->colorG,
                                                      gameOverScreen->continueText->colorB,
                                                      gameOverScreen->continueText->colorA);
    }
}

static void CustomMenuRender(feCustomMenuMgr* menuMgr) {
    Game::DrawEverythingHandlerCB(nullptr);
    DrawDirectorOverlays(nullptr);
    if (g_hud) {
        g_hud->Display();
    }
    if (menuMgr) {
        menuMgr->Render();
    }
}

static s32 CustomMenuDraw(feCustomMenuMgr* menuMgr) {
    s32 result = menuMgr ? menuMgr->Invoke() : 1;
    feCustomMenuMgr* renderMgr = menuMgr;
    if (result == 8 || result == 4) {
        renderMgr->Deactivate();
        renderMgr = nullptr;
    }
    g_display->BeginFrame();
    CustomMenuRender(renderMgr);
    g_display->EndFrame();
    return result;
}

static void ApplyLocationMenuCloseSideEffects(feMenuMgr* menuMgr, s32 menuResult) {
    if (!menuMgr || menuMgr->feMode != 1) {
        return;
    }

    if (menuResult == 8) {
        if (menuMgr->frontEndVolume && menuMgr->humanoid) {
            menuMgr->frontEndVolume->HandleVolumeExit(menuMgr->humanoid);
        }
    }
    else if (menuMgr->frontEndVolume) {
        g_destSelectReturnPos = menuMgr->frontEndVolume->savedPos;
        g_destSelectReturnPosValid = true;
    }
}

#endif

// PSX: EndFrameHandler (GAME.CPP:2205, pri=-64 in handlerSet2)
// Noop on PSX - Display's dispEndFrameHandler does the real work.
void Game::EndFrameHandler(Handler*) {
    MARKFUNCTION(0x8002B420);
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: game end");
#endif
    // PC platform: flush TransformAnim objects deferred by FreeAnimMemory.
    // Runs after AnimateEverythingHandler (pri=-48), ensuring animations freed
    // during handlerSet1 (Director/Think) remain valid for UpdateJoints this frame.
    if (g_characterManager) {
        g_characterManager->FlushPendingFree();
    }
}

bool Game::Step() {
    MARKFUNCTION(0x8002B65C); // Step__4Game

    if (stateFunc)
        return stateFunc(this);
    return false;
}

void Game::SetState(GameState s) {
    MARKFUNCTION(0x8002C5AC); // SetState
    if (s == state)
        return;

    prevState = state;
    s32 idx = static_cast<s32>(s);
    stateFunc = sStateTable[idx];
    state = s;

#if HIGH_FPS_PLAY_PRESENTATION
    auto usesFixedLogicStep = [](GameState s) {
        return s == GameState::Play || s == GameState::EndLevelLoop;
    };
    if (usesFixedLogicStep(state) || usesFixedLogicStep(prevState)) {
        if (g_time) {
            g_time->ResetPlayPresentationState();
        }
    }
#endif

    // PSX: for TitleLoop(3), Play(8), EndGameLoop(26), require pad0 connected bit.
    if (state == GameState::TitleLoop
        || state == GameState::Play
        || state == GameState::EndGameLoop) {
        const bool pad0Connected = (g_inputManager && ((g_inputManager->controls[0].flags & 0x01) != 0));
        if (!pad0Connected) {
            SetState(GameState::Error);
        }
    }

    LOG("[Game] State: %d -> %d", static_cast<int>(prevState), static_cast<int>(state));
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    char trace[64];
    std::snprintf(trace, sizeof(trace), "state %d -> %d",
                  static_cast<int>(prevState), static_cast<int>(state));
    PspPerf::Trace(trace);
#endif
}

bool Game::gsNullState(Game*) {
    MARKFUNCTION(0x80029328); // gsNullState
    return true;
}

bool Game::gsIntroState(Game* game) {
    MARKFUNCTION(0x800C99A0); // gsIntroState

#if CUSTOM_MENU
    // g_frontEndSound is normally constructed later in gsTitleState; without it, every
    // menu sound (open/move/confirm) in any of this state's boot-time popups is silently
    // dropped. Needed unconditionally - both the legal splash and the asset-missing gate
    // use g_feCustomMenuMgr regardless of whether AUTO_UPDATER is enabled.
    if (!g_feCustomMenuMgr) {
        InitXconFSImage();
        g_feCustomMenuMgr = new feCustomMenuMgr();
        g_feCustomMenuMgr->Init(&g_customText);
    }
    if (!g_psxDiscExtractor) {
        g_psxDiscExtractor = new PsxDiscExtractor();
        g_psxDiscExtractor->Init();
    }
#endif

#if SKIP_INTRO
    // Only skips the legal-splash presentation, not the asset check below - a build with
    // no PSX assets must never silently fall through to a broken title screen.
    game->introPhase = 4;
#endif

#if CUSTOM_MENU
    // Custom legal splash (entirely replaces the PSX LICENSE.TIM + movie intro)
    if (!game->assetCheckDone && game->introPhase != 4) {
        static constexpr f32 kLegalBlackSec = 0.25f;
        static constexpr f32 kLegalFadeInSec = 0.5f;
        static constexpr f32 kLegalHoldSec = 5.0f;
        static constexpr f32 kLegalFadeOutSec = 0.5f;

        const f32 dt = g_time ? g_time->GetDeltaTime() : (1.0f / 30.0f);
        game->customIntroTimer += dt;

        f32 alpha = 0.0f;
        bool finished = false;

        switch (game->introPhase) {
            case 0: // black hold
                alpha = 0.0f;
                if (game->customIntroTimer >= kLegalBlackSec) {
                    game->customIntroTimer = 0.0f;
                    game->introPhase = 1;
                }
                break;
            case 1: // fade in
                alpha = (kLegalFadeInSec > 0.0f) ? (game->customIntroTimer / kLegalFadeInSec) : 1.0f;
                if (game->customIntroTimer >= kLegalFadeInSec) {
                    game->customIntroTimer = 0.0f;
                    game->introPhase = 2;
                }
                break;
            case 2:
            { // hold at full visibility
                alpha = 1.0f;
                const bool skipPressed = g_actionInput &&
                    (g_actionInput->AnyJustPressed() ||
                     g_actionInput->IsMouseButtonTriggered(MouseBtn::Left) ||
                     g_actionInput->IsMouseButtonTriggered(MouseBtn::Right) ||
                     g_actionInput->IsMouseButtonTriggered(MouseBtn::Middle));
                if (skipPressed || game->customIntroTimer >= kLegalHoldSec) {
                    game->customIntroTimer = 0.0f;
                    game->introPhase = 3;
                }
                break;
            }
            case 3: // fade out
                alpha = (kLegalFadeOutSec > 0.0f) ? (1.0f - game->customIntroTimer / kLegalFadeOutSec) : 0.0f;
                if (game->customIntroTimer >= kLegalFadeOutSec) {
                    finished = true;
                }
                break;
            default:
                finished = true;
                break;
        }

        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;

        g_display->BeginFrame();
        if (g_feCustomMenuMgr) {
            g_feCustomMenuMgr->DrawLegalScreen(alpha);
        }
        g_display->EndFrame();

        if (finished) {
            game->introPhase = 4;
            game->customIntroTimer = 0.0f;
        }
        return true;
    }

    // --- Asset presence check + missing-assets gate ---
    if (!game->assetCheckDone) {
        // Once the popup is active, an in-progress extraction can make this sentinel
        // check (xc/fe/tim/license.tim) go true well before bin/sound is fully written
        // (disc LBA order, not whitelist order) - don't let that short-circuit the gate
        // and skip the post-extraction Sound::SetupSound() re-run below. Only the menu's
        // own Done state (menuResult == 8) may resolve the gate once it's active.
        const bool assetMenuActive = g_feCustomMenuMgr && g_feCustomMenuMgr->IsActive();
        if (!assetMenuActive && PsxDiscExtractor::AreAssetsPresent()) {
            game->assetCheckDone = true;
            game->introPhase = 0;
        }
        else {
            if (g_feCustomMenuMgr && !g_feCustomMenuMgr->IsActive()) {
                g_feCustomMenuMgr->Activate(MenuPage_AssetMissing);
            }

            if (g_feCustomMenuMgr) {
                s32 menuResult = g_feCustomMenuMgr->Invoke();

                g_display->BeginFrame();
                g_feCustomMenuMgr->Render();
                g_display->EndFrame();

                if (menuResult == 8) {
                    g_feCustomMenuMgr->Deactivate();
                    game->assetCheckDone = true;
                    game->introPhase = 0;

                    // Sound::SetupSound() already ran once in Game::InternalOpen(), before
                    // this extraction step could have populated bin/sound - the WAX banks it
                    // tried to load didn't exist yet. Re-run it now that they're on disk.
                    if (g_sound) {
                        g_sound->SetupSound();
                    }
                }
            }
            return true;
        }
    }
#else
    game->assetCheckDone = true;
#endif

#if CUSTOM_MENU
    if (!game->autosaveNoticeShown) {
        game->autosaveNoticeShown = true;
        if (g_feCustomMenuMgr) {
            g_feCustomMenuMgr->Activate(MenuPage_None);
            g_feCustomMenuMgr->OpenPopup(PopupKind_AutosaveNotice, 2.5f, []() -> s32 {
                return (s32)GameResult::ResumePlay;
            });
        }
    }
    if (g_feCustomMenuMgr && g_feCustomMenuMgr->IsActive()
        && g_feCustomMenuMgr->GetActivePopup() == PopupKind_AutosaveNotice) {
        const s32 menuResult = g_feCustomMenuMgr->Invoke();
        g_display->BeginFrame();
        g_feCustomMenuMgr->Render();
        g_display->EndFrame();
        if (menuResult == (s32)GameResult::ResumePlay) {
            g_feCustomMenuMgr->Deactivate();
        }
        return true;
    }
#endif

#if AUTO_UPDATER
    if (!g_autoUpdater) {
        g_autoUpdater = new AutoUpdater();
        g_autoUpdater->Init();
        g_autoUpdater->CheckAsync();
#if CUSTOM_MENU
        if (g_feCustomMenuMgr) {
            g_feCustomMenuMgr->Activate(MenuPage_None);
            g_feCustomMenuMgr->OpenPopup(PopupKind_CheckingUpdate, 1.0f, []() -> s32 {
                if (!g_autoUpdater || !g_autoUpdater->IsCheckComplete())
                    return -1;
                return (g_feCustomMenuMgr->GetCurrentPage() == MenuPage_None) ? (s32)GameResult::ResumePlay : 1;
            });
        }
#endif
    }

#if CUSTOM_MENU
    // Gates the rest of the intro on the checking popup until it resolves (dismissed,
    // installed, or no update found).
    if (g_feCustomMenuMgr && g_feCustomMenuMgr->IsActive()) {
        s32 menuResult = g_feCustomMenuMgr->Invoke();

        g_display->BeginFrame();
        g_feCustomMenuMgr->Render();
        g_display->EndFrame();

        if (menuResult == 8 || menuResult == 4) {
            g_feCustomMenuMgr->Deactivate();
        }
        return true;
    }
#endif
#endif

#if CUSTOM_MENU
    game->SetState(GameState::Init);
    return true;
#else
    // Phase 0: first entry - start 300-frame wait with LICENSE.TIM
    if (game->introPhase == 0) {
        game->introTimer = 0;
        TimImage* img = Tim::LoadFromFile("LICENSE.TIM");
        if (img) {
            game->introTexture = Tim::CreateTexture(img);
            delete img;
        }
        game->introPhase = 1;
    }

    // Phase 1: 300-frame wait (any button breaks)
    if (game->introPhase == 1) {
        g_display->BeginFrame();
        if (game->introTexture) {
            ScreenDraw::DrawFullscreen(game->introTexture);
        }
        g_display->EndFrame();
        game->introTimer++;

        u32 buttons = 0;
        if (g_inputManager) {
            g_inputManager->Step();
            buttons = g_inputManager->GetControlVal(0);
        }
        game->controlVal[0] = (s32)buttons;

        if (game->introTimer >= 300 || buttons != 0) {
            if (game->introTexture) {
                game->introTexture->Release();
                game->introTexture = nullptr;
            }

            game->PlayMovie("Mdwy320m.str", 1, 0);
            game->PlayMovie("radi.str", 1, 0);
            game->PlayMovie("dolby.str", 0, 0);

            game->introPhase = 0;
            game->introTimer = 0;

            // PSX: setup display RECT, GTE stereo, SetupEnv
            game->SetState(GameState::Init);
        }
        return true;
    }

    return true;
#endif
}

bool Game::gsTitleState(Game* game) {
    MARKFUNCTION(0x8002C474); // gsTitleState

    // PSX: FreeXconFE, InitXconFSImage
    FreeXconFE();
    InitXconFSImage();

    // PSX: destroy old screen manager, create TitleScreen(56)
    if (game->titleScreen) {
        delete game->titleScreen;
        game->titleScreen = nullptr;
    }
    game->titleScreen = new TitleScreen();
    game->titleScreen->Init("XC/TITLE.1", g_oxFontFile);

#if CUSTOM_MENU
    if (!g_feCustomMenuMgr) {
        g_feCustomMenuMgr = new feCustomMenuMgr();
        g_feCustomMenuMgr->Init(&g_customText);
    }
    g_feCustomMenuMgr->ResetTitleIntro();
#endif

    if (g_inputManager) {
        g_inputManager->Step();
        g_inputManager->GetControlVal(0);
    }



    // PSX: rsEvent(4, 22, 0, 0) - set sound location to title music
    rsEvent(RS_SET_LOCATION, 22, 0, 0);
    // PSX: rsEvent(5, 0, 0, 0) - start music
    rsEvent(RS_LEVEL_BEGIN, 0, 0, 0);

    // PSX: reset idle timers
    game->titleIdleTimer = 0;
    game->titleIdleBase = 0;
    game->titleIdleAccumSec = 0.0f;
    game->titleStartLatch = false;

    // PSX: ClearEasterEggs()

    p3d::context->SetClearColour(pddiColour(0, 0, 0));
    game->titleFadeType = 0;

    game->SetState(GameState::TitleLoop);

    if (g_inputManager) {
        const s16* titleMode = TitleControlModeArray();
        for (s16 padIndex = 0; padIndex < 2; ++padIndex) {
            g_inputManager->SetControlModeArray(padIndex, titleMode);
        }
    }

    return true;
}

bool Game::gsTitleLoopState(Game* game) {
    MARKFUNCTION(0x8002BE0C); // gsTitleLoopState

    // PSX: 1056 bytes, 73 blocks.
    // BeginFrame -> oxScreenManager Update+Render -> EndFrame
    // InputStep -> GetControlVal(0) -> idle timer check (900 frames)
    // Attract mode: rsEvent(6) -> FadeBegin -> fade loop -> FadeEnd -> rsEvent(3)
    //   -> PlayMovie("demo.str") -> ReloadFont -> gsTitleState
    // Normal: Start check -> PrintEasterEggs -> easter egg dispatch
    // Start press: ProcessSoundEvent(gp[72], 8) -> rsEvent(6) -> FadeBegin
    //   -> fade loop -> FadeEnd -> rsEvent(3) -> gp[28]=1 -> destroy screenMgr
    //   -> FreeXconFSImage -> LoadXconFE -> PlayMovie("prolog.str")
    //   -> LoadCharTexture(0) -> SetState(OpenFE=5)

    // PSX uses blocking inline fade loops. PC animates one step per frame.
    if (game->titleFadeType != 0) {
        // Continue rendering title screen behind the fade overlay
        g_display->BeginFrame();
        if (game->titleScreen) {
            game->titleScreen->Update();
#if CUSTOM_MENU
            game->RenderTitleWithCustomBackground(false);
#else
            game->titleScreen->Render();
#endif
        }

        s32 stillFading = FadeUpdate();
        FadeRender();
        g_display->EndFrame();

        if (!stillFading) {
            FadeEnd();

            if (game->titleFadeType == 2) {
                rsEvent(RS_UNLOAD_LEVEL, 0, 0, 0);
                game->PlayMovie("demo.str", 1, 0);
                LOG("[Game] TitleLoop: attract fade complete");
                game->titleIdleBase = 0;
                game->titleIdleTimer = 0;
                game->titleIdleAccumSec = 0.0f;
                game->titleFadeType = 0;
                if (!g_oxFontFile) g_oxFontFile = new oxFontFile();
                g_oxFontFile->ReloadFont("XC/FONTS.1");
                gsTitleState(game);
            }
            else {
                rsEvent(RS_UNLOAD_LEVEL, 0, 0, 0);
                game->field136 = 1;
                if (game->titleScreen) {
                    delete game->titleScreen;
                    game->titleScreen = nullptr;
                }
                FreeXconFSImage();
                LoadXconFE();
                if (!SaveGameHasPendingLoad()) {
                    game->PlayMovie("prolog.str", 1, 0);
                    g_frontEndSound->ProcessSoundEvent(FE_SND_MENU_ACCEPT);
                }
                if (g_characterManager) {
                    g_characterManager->LoadCharTexture((u32)AITypes::TT_PLAYER);
                }
                LOG("[Game] TitleLoop: fade complete -> OpenFE");
                game->titleFadeType = 0;
                game->SetState(GameState::OpenFE);
            }
        }
        return true;
    }

    u32 buttons = 0;
    if (g_inputManager) {
        g_inputManager->Step();
        buttons = g_inputManager->GetControlVal(0);
    }
    game->controlVal[0] = (s32)buttons;
    const bool startDown = ((buttons & PsxPad::Start) != 0);

    if (game->titleAutoStart) {
        rsEvent(RS_STOP_MUSIC, 0, 0, 0);
        rsEvent(RS_UNLOAD_LEVEL, 0, 0, 0);
        game->field136 = 1;
        if (game->titleScreen) {
            delete game->titleScreen;
            game->titleScreen = nullptr;
        }
        FreeXconFSImage();
        LoadXconFE();
        game->titleAutoStart = false;
        if (!SaveGameHasPendingLoad()) {
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
            // The profile target must reach QueueLevelLoad without waiting for
            // the optional prolog movie; release keeps the original flow.
            LOG("[Game] TitleLoop: profile skips prolog movie");
#else
            game->PlayMovie("prolog.str", 1, 0);
#endif
        }
        if (g_characterManager) {
            g_characterManager->LoadCharTexture((u32)AITypes::TT_PLAYER);
        }
        g_frontEndSound->ProcessSoundEvent(FE_SND_MENU_ACCEPT);
        LOG("[Game] TitleLoop: auto-start -> OpenFE");
        game->titleFadeType = 0;
        game->SetState(GameState::OpenFE);
        return true;
    }

#if CUSTOM_MENU
    if (g_feCustomMenuMgr && g_feCustomMenuMgr->IsActive()) {
        s32 menuResult = g_feCustomMenuMgr->Invoke();
        feCustomMenuMgr* renderMgr = g_feCustomMenuMgr;
        if (menuResult == 8 || menuResult == 4) {
            renderMgr->Deactivate();
            renderMgr = nullptr;
            if (menuResult == 8) {
                g_feCustomMenuMgr->ResetTitleIntro();
            }
            else if (menuResult == 4) {
                g_feCustomMenuMgr->HideTitleContent();
            }
        }

        g_display->BeginFrame();
        if (game->titleScreen) {
            game->RenderTitleWithCustomBackground(false);
        }
        if (renderMgr) {
            renderMgr->Render();
        }
        g_display->EndFrame();

        if (menuResult == 4) {
            rsEvent(RS_STOP_MUSIC, 0, 0, 0);
            FadeBegin();
            game->titleFadeType = 1;
        }
        if (!startDown) {
            game->titleStartLatch = false;
        }
        return true;
    }

    // Press-start transition: fade the logo/Jackie out and zoom the prompt
    // in over a short window before actually activating the menu, so the
    // swap isn't an instant single-frame cut.
    if (g_feCustomMenuMgr && g_feCustomMenuMgr->IsTitleStartTransitionRunning()) {
        g_display->BeginFrame();
        if (game->titleScreen) {
            game->titleScreen->Update();
            game->RenderTitleWithCustomBackground(true);
        }
        g_display->EndFrame();

        if (g_feCustomMenuMgr->IsTitleStartTransitionFinished()) {
            g_feCustomMenuMgr->EndTitleStartTransition();
            g_feCustomMenuMgr->Activate(MenuPage_Title);
        }
        return true;
    }
#endif

    g_display->BeginFrame();
    if (game->titleScreen) {
        game->titleScreen->Update();
#if CUSTOM_MENU
        game->RenderTitleWithCustomBackground(true);
#else
        game->titleScreen->Render();
#endif
#if AUTO_UPDATER
        if (g_feCustomMenuMgr) {
            g_feCustomMenuMgr->DrawVersionOverlay();
        }
#endif
    }
    g_display->EndFrame();

#if !CUSTOM_MENU
    // PSX: attract mode timer check (gp+128 - gp+124) >= 900
    s32 elapsed = game->titleIdleTimer - game->titleIdleBase;
    if (elapsed >= 900) {
        rsEvent(RS_STOP_MUSIC, 0, 0, 0);
        FadeBegin();
        game->titleFadeType = 2;
        return true;
    }
#endif

    if (startDown) {
#if CUSTOM_MENU
        if (g_feCustomMenuMgr) {
            if (!game->titleStartLatch) {
                if (g_frontEndSound) {
                    g_frontEndSound->ProcessSoundEvent(FE_SND_MENU_OPEN);
                }
                g_feCustomMenuMgr->BeginTitleStartTransition();
                game->titleIdleBase = game->titleIdleTimer;
                game->titleStartLatch = true;
            }
            return true;
        }
#endif
        // PSX: ProcessSoundEvent(gp[72], 8)
        if (g_frontEndSound) {
            g_frontEndSound->ProcessSoundEvent(FE_SND_MENU_OPEN);
        }
        // PSX: rsEvent(6, 0, 0, 0) - stop music
        rsEvent(RS_STOP_MUSIC, 0, 0, 0);
        FadeBegin();
        game->titleFadeType = 1;
    }
    else {
        game->titleStartLatch = false;

        game->titleIdleAccumSec += g_time ? g_time->GetDeltaTime() : 0.0f;
        const f32 kIdleFrameSec = 1.0f / 30.0f;
        while (game->titleIdleAccumSec >= kIdleFrameSec) {
            game->titleIdleAccumSec -= kIdleFrameSec;
            game->titleIdleTimer++;
        }
    }

    return true;
}

bool Game::gsInitState(Game* game) {
    MARKFUNCTION(0x80029460); // gsInitState

#if CUSTOM_MENU
    if (!g_feCustomMenuMgr) {
        g_feCustomMenuMgr = new feCustomMenuMgr();
        g_feCustomMenuMgr->Init(&g_customText);
    }
#endif

    // PSX: ClearImage, DisplayTIM(gp[24]), StartLogo(655360), FillMeter(100)
#if !CUSTOM_MENU && !defined(RC_PLATFORM_PSP)
    DisplayTIM("RUNFIRST.TIM");
#endif
    StartLogo("RUNFIRST.TIM");
    LOG("[Game] Init: RUNFIRST logo started");
    FillMeter(100);

    // PSX: if firstBoot (gp[80]): LoadPermanent(), clear firstBoot
    if (game->firstBoot) {
        // PSX: MEMSTAT(24, 1)
        World* world = game->GetWorld();
        if (world) {
            LOG("[Game] Init: LoadPermanent begin");
            world->LoadPermanent();
            LOG("[Game] Init: LoadPermanent complete");
        }
        // PSX: MEMSTAT(24, 2)
        game->firstBoot = 0;
    }

    if (g_inputManager) {
        LOG("[Game] Init: configure input begin");
        const s16* gameMode = GameControlModeArray();
        for (s16 padIndex = 0; padIndex < 2; ++padIndex) {
            g_inputManager->SetControlModeArray(padIndex, gameMode);
        }
        LOG("[Game] Init: configure input complete");
    }

    // PSX: VBlankLogo::StopLogo
    StopLogo();
    LOG("[Game] Init: logo stopped");

    game->SetState(GameState::Title);
    return true;
}

bool Game::gsOpenFEState(Game* game) {
    MARKFUNCTION(0x800299B8); // gsOpenFEState

    // PSX: first-time init - rMakePuddle for FE memory
    if (!g_feInitialized) {
        // PSX: rMakePuddle(cellAlligator, overlayAddr, 5836, 0)
        g_feInitialized = 1;
    }

    if (SaveGameApplyPendingLoad(game)) {
        if (g_time) {
            g_time->frameCounter = 0;
        }
        game->SetState(GameState::QueueLevelLoad);
        return true;
    }

    // PSX: check gp[44] (selectedLevel). If a level was previously
    // selected (e.g. returning from game over), go straight to loading.
    if (g_selectedLevel != -1) {
        World* world = game->GetWorld();
        if (world) {
#if defined(RC_PLATFORM_PSP) && defined(PSP_AUTO_START_PHASE1)
            // LoadPermanent has populated GAME_LN.TXT by the time OpenFE is
            // entered. Resolve the benchmark's stable level ID here instead
            // of doing it from main(), where the list is still empty and
            // LevelIDToIndex would legitimately fall back to index 0 (hub).
            g_selectedLevel = static_cast<s16>(world->LevelIDToIndex(1));
            PspPerf::WriteState("autostart", 1, g_selectedLevel, g_selectedLevel);
#endif
            world->SetTargetLevelPetal((u32)g_selectedLevel, 0);
        }
        if (g_time) {
            g_time->frameCounter = 0;
        }
        game->SetState(GameState::QueueLevelLoad);
    }
    else {
#if CUSTOM_MENU
        g_feCustomMenuMgr->Activate();
#endif
        // No level selected - show FE menu
        game->SetState(GameState::FE);
    }
    return true;
}

bool Game::gsFEState(Game* game) {
    MARKFUNCTION(0x80029A48); // gsFEState

    // PSX 0x80029A48: immediate transition to OpenLocationMenu.
    // FE menu interaction happens in LocationMenuState when applicable.
    game->SetState(GameState::OpenLocationMenu);
    return true;
}

bool Game::gsPrePlayState(Game* game) {
    MARKFUNCTION(0x80029AC0); // gsPrePlayState

    World* autosaveWorld = game->GetWorld();
    if (game->autosavePending && autosaveWorld && autosaveWorld->GetCurLevelID() == 7) {
        game->autosavePending = false;
        game->autosavePhase = 1;
        game->autosaveDelayFrames = 1; // ensure the wheel is presented before the synchronous write
        game->autosaveTimer = 0.0f;
    }

    // PSX: 432 bytes, 27 blocks.
    // Loads overlay, shows menu, sets up the audio listener, checks checkpoint,
    // resets HUD/input mappings, then transitions to Play state.
    // It does not call DetermineLevelIntro here; the Director entry script
    // seeded by World::Load handles that after Play begins.

    World* world = game->GetWorld();
    s32 levelID = (world) ? world->GetCurLevelID() : 0;

    // PSX: level 7 (hub): LoadOverlay(1), feMenuMgr->ShowNewGameMenu, feMenuMgr->OpenDoors
    // PSX: else: gameMenu->ShowPauseMenu(), LoadOverlay(0)
    if (levelID == 7) {
        // PSX: LoadOverlay(1) - load boss overlay for hub
        if (g_feMenuMgr) {
            g_feMenuMgr->ShowNewGameMenu();
            g_feMenuMgr->OpenDoors();
        }
    }
    else {
        if (g_gameMenu) {
            g_gameMenu->ShowPauseMenu();
        }
        // PSX: LoadOverlay(0) - load normal overlay
    }

    // PSX: theCamera->Think()
    if (g_display && g_display->GetCamera()) {
        g_display->GetCamera()->Think();

        if (levelID == 7 && g_arrowInside != 0 && Player::s_player) {
            Camera* camera = g_display->GetCamera();
            const LVector& camPos = camera->GetPosition();
            Player::s_player->FacePointDesired(camPos);
            Player::s_player->FacePoint(camPos, 0);
            Player::s_player->SetDesiredMoveDirection(Player::s_player->orientation.y);
        }
    }

    // PSX: rsEvent(21, player+28, theCamera+384, 0) - set 3D audio listener
    // The args are pointers to player position and camera matrix for 3D audio.
    // On PC we pass zeros - audio spatialization not yet wired.
    rsEvent(21, 0, 0, 0);

    // PSX: if CheckpointInfo::IsValid(player+636): theCamera->lookAtMode = 1
    if (Player::s_player && Player::s_player->checkpoint.IsValid()) {
        if (g_display && g_display->GetCamera()) {
            g_display->GetCamera()->SetLookAtMode(1);
        }
    }

    SaveGameApplyPendingLives();

    // PC
    if (g_display)
        g_display->SetCursorCaptured(true);

#if CUSTOM_MENU
#if defined(RC_PLATFORM_PSP)
    // The PSP front end already performed the loading transition.  The
    // desktop handler's time-based play fade can remain active when the
    // emulated VBlank clock is not advancing through this state, leaving a
    // fullscreen black quad over an otherwise valid world.  Keep the native
    // PSP path deterministic: enter gameplay fully revealed and let the
    // streaming music started by PreloadMusicTrack continue immediately.
    game->playFadeInActive = false;
    FadeEnd();
    g_deferLevelBeginMusic = false;
#else
    game->playFadeInActive = true;
    FadeBegin();
#endif
#endif

    // PSX: SetState(Play=8)
    game->SetState(GameState::Play);

    // PSX: g_directorActive = 1 (gp+20)
    g_directorActive = 1;

    // PSX: HUD->InternalReset()
    g_hud->InternalReset();

    // PSX: clear controlVal
    game->controlVal[0] = 0;
    game->controlVal[1] = 0;

    // PSX: loop i=0..1: SetControlModeArray, PlayerMapArray, SetControlMapArray
    if (g_inputManager) {
        const s16* gameMode = GameControlModeArray();
        const u8* playerMap = g_inputManager->PlayerMapArray();
        for (s16 padIndex = 0; padIndex < 2; ++padIndex) {
            g_inputManager->SetControlModeArray(padIndex, gameMode);
            g_inputManager->SetControlMapArray(padIndex, playerMap);
        }
    }

    // PSX: if level != 7: SetHUDVisible(0, 1, 1)
    if (g_hud) {
        World* world = game->GetWorld();
        if (world && world->GetCurLevelID() != 7) {
            g_hud->SetHUDVisible(1, 1);
        }
    }

    // PSX: MEMSTAT_NEW_PRINT, SetMemoryState(1)

    return true;
}

bool Game::gsPlayState(Game* game) {
    MARKFUNCTION(0x80029C6C); // gsPlayState

#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    static u32 pspPlayStateProbeFrames = 0;
    const bool probePspPlayState = pspPlayStateProbeFrames < 4;
    if (probePspPlayState) {
        LOG("[PSP Play] enter state=%d directorActive=%d", static_cast<int>(game->state), g_directorActive);
    }
#endif

    if (game->autosavePhase != 0) {
        const f32 dt = g_time ? g_time->GetDeltaTime() : (1.0f / 30.0f);
        if (game->autosavePhase == 1) {
            if (game->autosaveDelayFrames > 0) {
                --game->autosaveDelayFrames;
            }
            else if (SaveGameWriteAutosave()) {
                game->autosavePhase = 2;
                game->autosaveTimer = 2.0f;
            }
            else {
                LOG("[Autosave] Failed to write userfiles/jcsAUTOSAVE.sav");
                game->autosavePhase = 3;
                game->autosaveTimer = 2.0f;
            }
        }
        else {
            game->autosaveTimer -= dt;
            if (game->autosaveTimer <= 0.0f) {
                game->autosavePhase = 0;
                game->autosaveTimer = 0.0f;
            }
        }
    }

#if HIGH_FPS_PLAY_PRESENTATION
    s32 logicSteps = 1;
    if (g_time) {
        logicSteps = g_time->BeginPlayFixedStep();
    }

    if (g_directorActive) {
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("play step: director begin");
#endif
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspPlayState) LOG("[PSP Play] before director");
#endif
        for (s32 i = 0; i < logicSteps; ++i) {
            if (g_inputManager) {
                g_inputManager->CommitHostPads();
                g_inputManager->Step();
                for (s16 padIndex = 0; padIndex < 2; ++padIndex) {
                    game->controlVal[padIndex] = g_inputManager->GetControlVal((u16)padIndex);
                }
            }

            if (g_time) {
                g_time->Step();
            }
            if (g_director) {
                g_director->Process();
            }
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
            if (probePspPlayState) LOG("[PSP Play] after director");
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
            PspPerf::Trace("play step: director done");
#endif

            jcsUpdateDialogCD();
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
            if (probePspPlayState) LOG("[PSP Play] after dialog update");
#endif
            AdvanceGameplayAnimationOneTick();
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
            if (probePspPlayState) LOG("[PSP Play] after animation");
#endif
        }

        if (g_display && g_display->GetCamera()) {
#if defined(RC_PLATFORM_PSP) && defined(PSP_AUTO_START_PHASE1)
            DebugUI::UpdateBenchmarkCamera(g_display->GetCamera());
#endif
            g_display->GetCamera()->UpdateHighFPS();
        }
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspPlayState) LOG("[PSP Play] after camera");
#endif

#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspPlayState) LOG("[PSP Play] before spatial audio");
#endif
        UpdateSpatialAudioListener();
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspPlayState) {
            LOG("[PSP Play] after audio drawList head=%p tail=%p",
                static_cast<void*>(game->handlerSet2.handlerList.head),
                static_cast<void*>(game->handlerSet2.handlerList.tail));
        }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("play step: audio done");
#endif
        ProcessHandlerList(game->handlerSet2.handlerList, "play-draw");
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspPlayState) {
            LOG("[PSP Play] after draw handlers");
            ++pspPlayStateProbeFrames;
        }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("play step: handlers done");
#endif
        return true;
    }

    for (s32 i = 0; i < logicSteps; ++i) {
        if (g_inputManager) {
            g_inputManager->CommitHostPads();
            g_inputManager->Step();
            for (s16 padIndex = 0; padIndex < 2; ++padIndex) {
                game->controlVal[padIndex] = g_inputManager->GetControlVal((u16)padIndex);
            }
        }

        if (g_time) {
            g_time->Step();
        }
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspPlayState) LOG("[PSP Play] before logic handlers");
#endif
        ProcessHandlerList(game->handlerSet1.handlerList);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspPlayState) LOG("[PSP Play] after logic handlers");
#endif

        jcsUpdateDialogCD();
        AdvanceGameplayAnimationOneTick();
    }

    // PSX pause gate: director script must be idle. Keep this on logic ticks only.
    if (logicSteps > 0 && game->state == GameState::Play) {
        s32 canPause = (!g_director || g_director->scriptState == 0);
        if (canPause && (game->controlVal[0] & PsxPad::Start) != 0) {
#if CUSTOM_MENU
            {
                World* world = game->GetWorld();
                const bool isHub = (world && world->GetCurLevelID() == 7);
                g_feCustomMenuMgr->Activate(isHub ? MenuPage_Frontend : MenuPage_Pause);
            }
#endif

            LOG("[Game] Pause requested from Play");
            game->SetState(GameState::Menu);
            Shock(ShockEnum::SHOCK_CLEAR);
        }
    }

    if (g_display && g_display->GetCamera()) {
#if defined(RC_PLATFORM_PSP) && defined(PSP_AUTO_START_PHASE1)
        DebugUI::UpdateBenchmarkCamera(g_display->GetCamera());
#endif
        g_display->GetCamera()->UpdateHighFPS();
    }

#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    if (probePspPlayState) LOG("[PSP Play] after camera");
#endif

    UpdateSpatialAudioListener();

#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    if (probePspPlayState) LOG("[PSP Play] after spatial audio");
#endif

    // PSX: CInteractiveMusicController::Think (MSCCTRLR.CPP:56) - per-frame FAG song switching.
    InteractiveMusicControllerThink();

#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    if (probePspPlayState) LOG("[PSP Play] after music");
#endif

    ProcessHandlerList(game->handlerSet2.handlerList, "play-draw");
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    if (probePspPlayState) {
        LOG("[PSP Play] after draw handlers");
        ++pspPlayStateProbeFrames;
    }
#endif
    return true;
#else
    if (g_directorActive) {
        if (g_director) {
            g_director->Process();
        }

        UpdateSpatialAudioListener();
        ProcessHandlerList(game->handlerSet2.handlerList);
        return true;
    }

    // PSX: InputManager::Step, then loop 2 pads storing GetControlVal
    if (g_inputManager) {
        g_inputManager->Step();
        for (s16 padIndex = 0; padIndex < 2; ++padIndex) {
            game->controlVal[padIndex] = g_inputManager->GetControlVal((u16)padIndex);
        }
    }

    // PSX: ProcessHandlers(game) - runs handlerSet1 (think) + handlerSet2 (draw)
    game->ProcessHandlers();
    UpdateSpatialAudioListener();

    // PSX: CInteractiveMusicController::Think (MSCCTRLR.CPP:56) - per-frame FAG song switching.
    InteractiveMusicControllerThink();

    // PSX: check state==Play AND director scriptState==0 for pause eligibility
    if (game->state == GameState::Play) {
        s32 canPause = (!g_director || g_director->scriptState == 0);
        if (canPause && (game->controlVal[0] & PsxPad::Start) != 0) {
#if CUSTOM_MENU
            {
                World* world = game->GetWorld();
                const bool isHub = (world && world->GetCurLevelID() == 7);
                g_feCustomMenuMgr->Activate(isHub ? MenuPage_Frontend : MenuPage_Pause);
            }
#endif

            LOG("[Game] Pause requested from Play");
            game->SetState(GameState::Menu);
            Shock(ShockEnum::SHOCK_CLEAR);
        }
    }

    // PSX: Update__7Profile() - PSX profiling system, not applicable on PC
    return true;
#endif
}

bool Game::gsEndLevelState(Game* game) {
    MARKFUNCTION(0x8002B688); // gsEndLevelState
    game->SetState(GameState::EndLevelLoop);
    return true;
}

bool Game::gsEndLevelLoopState(Game* game) {
    MARKFUNCTION(0x8002B6B0); // gsEndLevelLoopState

#if HIGH_FPS_PLAY_PRESENTATION
    const s32 logicSteps = g_time ? g_time->GetLogicStepCount() : 1;
    for (s32 i = 0; i < logicSteps; ++i) {
        animLoopDSTACK();
    }
#else
    animLoopDSTACK();
#endif

    MenuDraw(nullptr);

    if (!g_hud || (!g_hud->visible && !g_hud->takes.isPlaying && !g_hud->dragon.isPlaying)) {
        game->SetState(GameState::EndLevelExit);
    }

    return true;
}

bool Game::gsEndLevelExitState(Game* game) {
    MARKFUNCTION(0x8002B744); // gsEndLevelExitState

    if (g_scoreManager) {
        g_scoreManager->HandleLevelEnd();
    }

    World* world = game->GetWorld();
    if (!world) {
        game->SetState(GameState::QueueLevelLoad);
        return true;
    }

    s32 targetLevelID = 7;
    const s32 nextPetal = (s32)world->GetCurrentPetalIndex() + 1;

    if (nextPetal < world->GetCurLevelPetals()) {
        g_scoreManager->OpenPetal(
            (u32)world->GetCurrentLevelIndex(),
            (u32)nextPetal
        );
    }
    else {
        const s32 currentLevelID = world->GetCurLevelID();
        switch (currentLevelID) {
            case 1:
                targetLevelID = 11;
                break;
            case 2:
                targetLevelID = 12;
                break;
            case 3:
                targetLevelID = 13;
                break;
            case 4:
                targetLevelID = 14;
                break;
            case 5:
                rsEvent((rsSoundEvent)6, 0, 0, 0);
                MenuFade();
                targetLevelID = 8;
                game->PlayMovie("factory.str", 1, 1);
                break;
            case 6:
                if (g_scoreManager->GetTotalGoldDragon() >= 20) {
                    rsEvent((rsSoundEvent)6, 0, 0, 0);
                    MenuFade();
                    game->PlayMovie("making.str", 1, 1);
                }
                break;
            case 8:
            {
                const s32 level6Index = world->LevelIDToIndex(6);
                g_scoreManager->OpenPetal((u32)level6Index, 0);

                rsEvent((rsSoundEvent)6, 0, 0, 0);
                MenuFade();

                game->PlayMovie("victory.str", 1, 1);
                game->PlayMovie("credits.str", 1, 1);
                targetLevelID = 7;
                break;
            }
            case 11:
            {
                const s32 level2Index = world->LevelIDToIndex(2);
                g_scoreManager->OpenPetal((u32)level2Index, 0);
                break;
            }
            case 12:
            {
                const s32 level3Index = world->LevelIDToIndex(3);
                g_scoreManager->OpenPetal((u32)level3Index, 0);
                break;
            }
            case 13:
            {
                const s32 level4Index = world->LevelIDToIndex(4);
                g_scoreManager->OpenPetal((u32)level4Index, 0);
                break;
            }
            case 14:
            {
                const s32 level5Index = world->LevelIDToIndex(5);
                g_scoreManager->OpenPetal((u32)level5Index, 0);
                break;
            }
            default:
                targetLevelID = 7;
                break;
        }
    }

    const s32 targetLevelIndex = world->LevelIDToIndex(targetLevelID);
    world->SetTargetLevelPetal((u32)targetLevelIndex, 0);

    if (targetLevelID == 7)
        game->autosavePending = true;
    game->SetState(GameState::QueueLevelLoad);
    return true;
}

bool Game::gsPlayMovieCredits(Game* game) {
    MARKFUNCTION(0x8002CB28); // gsPlayMovieCredits
    MenuFade();
    game->PlayMovie("credits.str", 1, 1);
    game->SetState(GameState::QueueLevelLoad);
    return true;
}

bool Game::gsDbgMenuState(Game*) {
    MARKFUNCTION(0x8002A174); // gsDbgMenuState
    return true;
}

bool Game::gsMenuState(Game* game) {
    MARKFUNCTION(0x80029EF8); // gsMenuState

    SyncPausedAnimationTicks();

#if CUSTOM_MENU
    s32 result = CustomMenuDraw(g_feCustomMenuMgr);
#else
    // PSX: GetCurLevelID(world)
    // PSX: level 7 (boss): menuMgr = gp[48] (feMenuMgr)
    // PSX: else: menuMgr = gp[52] (gameMenu)
    MenuMgr* menuMgr = g_gameMenu;
    World* world = game ? game->GetWorld() : nullptr;
    if (world && world->GetCurLevelID() == 7) {
        menuMgr = g_feMenuMgr;
    }

    // PSX: result = MenuDraw(menuMgr)
    s32 result = MenuDraw(menuMgr);
#endif
    if (result == 4 || result == 8) {
        LOG("[Game] Pause menu result=%d", result);
    }

    // PSX: result 8 = resume game (back to Play)
    if (result == 8) {
        game->SetState(GameState::Play);
    }
    // PSX: result 4 = quit game (callback already set state via SetState)

    return true;
}

bool Game::gsErrorState(Game* game) {
    MARKFUNCTION(0x80029F64); // gsErrorState
    game->SetState(GameState::ErrorLoop);
    return true;
}

bool Game::gsErrorLoopState(Game*) {
    MARKFUNCTION(0x8002A064); // gsErrorLoopState
    return true;
}

bool Game::gsErrorExitState(Game* game) {
    MARKFUNCTION(0x8002A004); // gsErrorExitState
    game->SetState(GameState::Title);
    return true;
}

bool Game::gsLocationMenuState(Game* game) {
    MARKFUNCTION(0x8002A128); // gsLocationMenuState

    // PSX 0x8002A128: result = MenuDraw(feMenuMgr); if result==8, return to Play.
    s32 menuResult = 1;

#if CUSTOM_MENU
    if (g_feCustomMenuMgr) {
        if (!g_feCustomMenuMgr->IsActive() || g_feCustomMenuMgr->GetCurrentPage() != MenuPage_Location) {
            g_feCustomMenuMgr->Activate(MenuPage_Location);
        }
        menuResult = CustomMenuDraw(g_feCustomMenuMgr);
    }
    else {
        menuResult = MenuDraw(g_feMenuMgr);
    }

    if (menuResult == 4 || menuResult == 8) {
        ApplyLocationMenuCloseSideEffects(g_feMenuMgr, menuResult);
    }
#else
    menuResult = MenuDraw(g_feMenuMgr);
#endif

    if (menuResult == 8) {
        if (g_feMenuMgr) {
            g_feMenuMgr->ShowNewGameMenu();
        }
        game->SetState(GameState::Play);
    }

    return true;
}

bool Game::gsOpenLocationState(Game* game) {
    MARKFUNCTION(0x80029A70); // gsOpenLocationState

    // PSX: world target = (level 6, petal 0), time->frameCounter = 0,
    // then QueueLevelLoad and ResetLevel.
    World* world = game->GetWorld();
    if (world) {
        world->SetTargetLevelPetal(6, 0);
        world->ResetLevel();
    }
    if (g_time) {
        g_time->frameCounter = 0;
    }

    g_selectedLevel = 6;
    game->SetState(GameState::QueueLevelLoad);
    return true;
}

bool Game::gsQueueLevelLoad(Game* game) {
    MARKFUNCTION(0x80029574); // gsQueueLevelLoad
    LOG("[Game] QueueLevelLoad: begin");

    // PSX: 520 bytes, 33 blocks.

    // PSX: rsEvent(6,0,0,0) - stop music
    rsEvent(RS_STOP_MUSIC, 0, 0, 0);

    // PSX: Shock(18) - controller vibration pulse
    Shock(ShockEnum::SHOCK_CLEAR);

    // Keep HUD hidden for the full transition fade on PC runtime.
    if (g_hud) {
        g_hud->SetHUDVisible(0, 1);
    }

    // PSX: MenuFade() - blocking fade to black
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("queue level: menu fade begin");
#endif
    MenuFade();
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("queue level: menu fade done");
#endif

    // PSX: SetHUDVisible(0, 0, 1) - hide HUD during load
    if (g_hud) {
        g_hud->SetHUDVisible(0, 1);
    }

    // PSX: SetMemoryState(0), MEMSTAT_CLEAR, MEMSTAT_MIN_CLEAR, MEMSTAT_NEW_RESET
    // PSX memory tracking - not applicable on PC

    World* world = game->GetWorld();
    if (!world) {
        game->SetState(GameState::Error);
        return true;
    }

#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY) && defined(PSP_AUTO_START_PHASE1)
    PspPerf::WriteState("load_begin", world->GetLevelIDFromIndex(world->GetTargetLevelIndex()),
        static_cast<s32>(world->GetCurrentLevelIndex()),
        static_cast<s32>(world->GetTargetLevelIndex()));
#endif

    // PSX: UnloadLevel(world), UnloadLevelPart2(world)
    world->Unload();
    LOG("[Game] QueueLevelLoad: world unload complete");
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY) && defined(PSP_AUTO_START_PHASE1)
    PspPerf::WriteState("unload_done", world->GetLevelIDFromIndex(world->GetTargetLevelIndex()),
        static_cast<s32>(world->GetCurrentLevelIndex()),
        static_cast<s32>(world->GetTargetLevelIndex()));
#endif
    world->UnloadLevelPart2();
    LOG("[Game] QueueLevelLoad: level part2 unload complete");
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY) && defined(PSP_AUTO_START_PHASE1)
    PspPerf::WriteState("unload2_done", world->GetLevelIDFromIndex(world->GetTargetLevelIndex()),
        static_cast<s32>(world->GetCurrentLevelIndex()),
        static_cast<s32>(world->GetTargetLevelIndex()));
#endif

    // PSX: FreeDynamicPrimBuffers(), AllocateDynamicPrimBuffers(0)
    // PSX GPU primitive buffers - not applicable on PC

    // PSX: DisplayTIM(gp[24]) - show loading screen background
#if CUSTOM_MENU
#if !defined(RC_PLATFORM_PSP)
    if (!g_feCustomMenuMgr) {
        DisplayTIM("RUNFIRST.TIM");
    }
#endif
#else
#if !defined(RC_PLATFORM_PSP)
    DisplayTIM("RUNFIRST.TIM");
#endif

    // PSX: ShowLoadingScreenText(gameMenu, levelId, petalTarget)
    if (g_gameMenu) {
        g_gameMenu->ShowLoadingScreenText(world->GetTargetLevelIndex(), world->GetTargetPetalIndex());
    }
#endif

#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY) && defined(PSP_AUTO_START_PHASE1)
    PspPerf::WriteState("loading_screen", world->GetLevelIDFromIndex(world->GetTargetLevelIndex()),
        static_cast<s32>(world->GetCurrentLevelIndex()),
        static_cast<s32>(world->GetTargetLevelIndex()));
#endif

    // PSX: LoadOverlay(levelType==7) - load code overlay
    // PSX: BossAI overlay switch for levels 1-7 vs 8,11-14
    // Not applicable on PC - all code is statically linked

#if CUSTOM_MENU
    g_deferLevelBeginMusic = true;
#endif

    // PSX: LoadLevel(world, targetLevelIndex) - internally calls Construct
    // which spawns AI entities, resets Director, sets level script
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("queue level: world load begin");
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY) && defined(PSP_AUTO_START_PHASE1)
    PspPerf::WriteState("world_load_begin", world->GetLevelIDFromIndex(world->GetTargetLevelIndex()),
        static_cast<s32>(world->GetCurrentLevelIndex()),
        static_cast<s32>(world->GetTargetLevelIndex()));
#endif
    if (!world->LoadLevelIndex(world->GetTargetLevelIndex())) {
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY) && defined(PSP_AUTO_START_PHASE1)
        PspPerf::WriteState("load_fail", world->GetLevelIDFromIndex(world->GetTargetLevelIndex()),
            static_cast<s32>(world->GetCurrentLevelIndex()),
            static_cast<s32>(world->GetTargetLevelIndex()));
#endif
        StopLogo();
        game->SetState(GameState::Error);
        return true;
    }
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY) && defined(PSP_AUTO_START_PHASE1)
    PspPerf::WriteState("load_done", world->GetCurLevelID(),
        static_cast<s32>(world->GetCurrentLevelIndex()),
        static_cast<s32>(world->GetTargetLevelIndex()));
#endif
    LOG("[Game] QueueLevelLoad: level load complete");
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("queue level: world load done");
#endif

    // PSX: Camera setup data is prepared during World::LoadLevelIndex
    // via ExecuteLoadCallbacks -> cameraLoadFunc -> SetupPaths.
    g_display->GetCamera()->Reset();
    if (g_cameraManager) {
        g_display->GetCamera()->SetCameraAnchor(g_cameraManager->GetAnchor());
    }

    tMatrixCamera* cam = g_display->GetCamera()->GetP3DCamera();
    cam->SetNearPlane(Camera::kRenderNearPlane);
    cam->SetFarPlane(65535.0f);

    g_display->GetView().SetCamera(g_display->GetCamera()->GetP3DCamera());
    g_display->GetView().SetBackgroundColour(pddiColour(0, 0, 0));
    g_display->GetView().SetClearMask(PDDI_BUFFER_ALL);

    // PSX: SetLookAtTarget via CameraManager path system.
    // Player was created by AI::Populate inside World::LoadLevelIndex -> Construct.
    if (Player::s_player) {
        g_display->GetCamera()->SetLookAtTarget(Player::s_player, 1);
    }

    // PSX: SetState(PrePlay=7)
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("queue level: preplay");
#endif
    game->SetState(GameState::PrePlay);

    // PSX: jcsStartDialog() - initialize dialog/subtitle system
    jcsStartDialog();

    // PSX: Step__12InputManager(0); GetControlVal__12InputManagerUs(0, 0)
    if (g_inputManager) {
        g_inputManager->Step();
        g_inputManager->GetControlVal(0);
    }

    return true;
}

bool Game::gsQueuePetalLoad(Game* game) {
    MARKFUNCTION(0x8002977C); // gsQueuePetalLoad

    // PSX: rsEvent(6,0,0,0) - stop music
    rsEvent(RS_STOP_MUSIC, 0, 0, 0);

    // PSX: Shock(18), MenuFade(), SetHUDVisible(0, 0, 1)
    Shock(ShockEnum::SHOCK_CLEAR);
    if (g_hud) {
        g_hud->SetHUDVisible(0, 1);
    }
    MenuFade();
    if (g_hud) {
        g_hud->SetHUDVisible(0, 1);
    }
    // PSX: SetMemoryState(0), MEMSTAT_CLEAR()

    World* world = game->GetWorld();
    if (!world) {
        game->SetState(GameState::Error);
        return true;
    }

    // PSX: UnloadPetal(world)
    world->UnloadPetal();

    // PSX: DisplayTIM(gp[24])
#if CUSTOM_MENU
#if !defined(RC_PLATFORM_PSP)
    if (!g_feCustomMenuMgr) {
        DisplayTIM("RUNFIRST.TIM");
    }
#endif
#else
#if !defined(RC_PLATFORM_PSP)
    DisplayTIM("RUNFIRST.TIM");
#endif
#endif

    // PSX: ShowLoadingScreenText(gameMenu, currentLevelIndex, targetPetalIndex)
#if !CUSTOM_MENU
    if (g_gameMenu) {
        g_gameMenu->ShowLoadingScreenText(world->GetCurrentLevelIndex(), world->GetTargetPetalIndex());
    }
#endif

#if CUSTOM_MENU
    g_deferLevelBeginMusic = true;
#endif

    // PSX: LoadPetal(world, targetPetalIndex)
    world->LoadPetal(world->GetTargetPetalIndex());

    // PSX: Camera setup data is prepared during World::LoadPetal
    // via ExecuteLoadCallbacks -> cameraLoadFunc -> SetupPaths.
    g_display->GetCamera()->Reset();
    if (g_cameraManager) {
        g_display->GetCamera()->SetCameraAnchor(g_cameraManager->GetAnchor());
    }
    if (Player::s_player) {
        g_display->GetCamera()->SetLookAtTarget(Player::s_player, 1);
    }

    // PSX: SetState(PrePlay=7)
    game->SetState(GameState::PrePlay);

    // PSX: jcsStartDialog()
    jcsStartDialog();

    // PSX: Step__12InputManager(0); GetControlVal__12InputManagerUs(0, 0)
    if (g_inputManager) {
        g_inputManager->Step();
        g_inputManager->GetControlVal(0);
    }

    return true;
}

bool Game::gsQueueLevelPetalLoad(Game* game) {
    MARKFUNCTION(0x8002986C); // gsQueueLevelPetalLoad

    // PSX chooses QueueLevelLoad (20) vs QueuePetalLoad (21) from
    // current/target world level+petal indices. ResetLevel is called
    // when any target differs from current.
    World* world = game->GetWorld();
    if (!world) {
        game->SetState(GameState::QueueLevelLoad);
        return true;
    }

    if (world->GetCurrentLevelIndex() != world->GetTargetLevelIndex()) {
        game->SetState(GameState::QueueLevelLoad);
        world->ResetLevel();
    }
    else if (world->GetCurrentPetalIndex() != world->GetTargetPetalIndex()) {
        game->SetState(GameState::QueuePetalLoad);
        world->ResetLevel();
    }
    else {
        game->SetState(GameState::QueuePetalLoad);
    }

    // PSX: jcsStartDialog()
    jcsStartDialog();

    // PSX: Step__12InputManager(0); GetControlVal__12InputManagerUs(0, 0)
    if (g_inputManager) {
        g_inputManager->Step();
        g_inputManager->GetControlVal(0);
    }

    return true;
}

bool Game::gsDetermineNextGameState(Game* game) {
    MARKFUNCTION(0x80029924); // gsDetermineNextGameState

    // PSX: Shock(18) - controller vibration on death
    Shock(ShockEnum::SHOCK_CLEAR);

    // PSX: decrement lives and branch to EndGame if depleted, else QueuePetalLoad.
    if (Player::s_player) {
        s32 lives = Player::s_player->GetLivesLeft() - 1;
        Player::s_player->SetLivesLeft(lives);

        if (lives <= 0) {
            Player::s_player->SetLivesLeft(Player::kMaxLives);
            game->SetState(GameState::EndGame);
        }
        else {
            if (World* world = game->GetWorld()) {
                world->RequestPlayerResetOnLoad();
            }
            game->SetState(GameState::QueuePetalLoad);
        }
    }
    else {
        game->SetState(GameState::QueuePetalLoad);
    }

    return true;
}

bool Game::gsDetermineGameOverState(Game* game) {
    MARKFUNCTION(0x800299B0); // gsDetermineGameOverState
    // PSX: returns 0 (false) - stops the game loop
    return false;
}

bool Game::gsEndGameState(Game* game) {
    MARKFUNCTION(0x8002C3B4); // gsEndGameState

    // PSX: SetHUDVisible(hud, 0, 1), UnloadLevel(world), LoadOverlay(1)
    if (g_hud) {
        g_hud->SetHUDVisible(0, 1);
    }
    // PSX: FreeXconFE(), InitXconFSImage()
    FreeXconFE();
    InitXconFSImage();

#if CUSTOM_MENU
    if (!g_feCustomMenuMgr) {
        g_feCustomMenuMgr = new feCustomMenuMgr();
        g_feCustomMenuMgr->Init(&g_customText);
    }
#endif

    // PSX: new oxScreenManager(56) -> Init("xc/gameover.1", screenMgr)
    if (game->gameOverScreen) {
        delete game->gameOverScreen;
        game->gameOverScreen = nullptr;
    }
    game->gameOverScreen = new GameOverScreen();
    game->gameOverScreen->Init("XC/GAMEOVER.1", g_oxFontFile);

    // PSX: rsEvent(4, 23, 0, 0) - set sound location to 23 (game over music)
    rsEvent(RS_SET_LOCATION, 23, 0, 0);
    // PSX: rsEvent(5, 0, 0, 0) - start music
    rsEvent(RS_LEVEL_BEGIN, 0, 0, 0);

    game->gameOverFadeType = 0;

    game->SetState(GameState::EndGameLoop);
    return true;
}

bool Game::gsEndGameLoopState(Game* game) {
    MARKFUNCTION(0x8002C22C); // gsEndGameLoopState

    // PSX: BeginFrame -> oxScreenManager::Update + Render -> EndFrame
    g_display->BeginFrame();
    if (game->gameOverScreen) {
        game->gameOverScreen->Update();
#if CUSTOM_MENU
        game->RenderGameOverWithCustomBackground();
#else
        game->gameOverScreen->Render();
#endif
    }

    if (game->gameOverFadeType != 0) {
        s32 stillFading = FadeUpdate();
        FadeRender();
        g_display->EndFrame();

        if (!stillFading) {
            FadeEnd();
            // PSX: rsEvent(3,0,0,0), destroy screenMgr, FreeXconFSImage,
            // DeletePlayerBlendAndAnimData, LoadXconFE, SetState(OpenLocationMenu=19)
            rsEvent(RS_UNLOAD_LEVEL, 0, 0, 0);
            game->field136 = 1;
            if (game->gameOverScreen) {
                delete game->gameOverScreen;
                game->gameOverScreen = nullptr;
            }
            FreeXconFSImage();
            DeletePlayerBlendAndAnimData();
            LoadXconFE();
            game->SetState(GameState::OpenLocationMenu);
        }
        return true;
    }

    g_display->EndFrame();

    u32 buttons = 0;
    if (g_inputManager) {
        g_inputManager->Step();
        buttons = g_inputManager->GetControlVal(0);
    }
    game->controlVal[0] = (s32)buttons;

    bool continuePressed = (buttons & (PsxPad::Start | PsxPad::Cross)) != 0;

#if CUSTOM_MENU
    if (!continuePressed && g_actionInput) {
        continuePressed = g_actionInput->JustPressed(ACTION_MENU_CONFIRM) ||
                          g_actionInput->IsMouseButtonTriggered(MouseBtn::Left);
    }
#endif

    if (continuePressed) {
        // PSX: ProcessSoundEvent(frontEndSound, 19) = FE_SND_JT_0
        if (g_frontEndSound) {
            g_frontEndSound->ProcessSoundEvent(FE_SND_JT_0);
        }
        rsEvent(RS_STOP_MUSIC, 0, 0, 0);
        FadeBegin();
        game->gameOverFadeType = 1;
    }

    return true;
}

bool Game::gsEndState(Game*) {
    MARKFUNCTION(0x8002A17C); // gsEndState
    return false;
}

// PSX: PlayMovie__4GamePcii (GAME.CPP:3309, 0x8002BBF0)
// Plays an STR movie file. Blocks until movie finishes or is skipped.
// PSX: func_800366E8 x4, func_80026C04 x2, func_80027638, then
//   if unloadLevel: UnloadLevel, LoadOverlay(1)
//   rsEvent(4,24,0,0), rsEvent(5,0,0,0), rsEvent(12,7,0,0)
//   new(348) MoviePlayer, SetPath, Play(callback)
//   if unloadLevel after: LoadOverlay(0), ReloadFont, LoadCharTexture
//   rsEvent(13,7,0,0), rsEvent(6,0,0,0), rsEvent(3,0,0,0)
void Game::PlayMovie(const char* name, s32 skippable, s32 unloadLevel) {
    MARKFUNCTION(0x8002BBF0);

    LOG("[Game] PlayMovie(\"%s\", skip=%d, unload=%d)", name, skippable, unloadLevel);

    // PSX: display sync / GTE setup calls (func_800366E8, func_80026C04, func_80027638)
    // PC: not needed

    // PSX: if (unloadLevel) { world->UnloadLevel(); LoadOverlay(1); }
    // PC: overlay system is not used; keep the world unload side-effect.
    if (unloadLevel) {
        World* world = GetWorld();
        if (world) {
            world->Unload();
        }
    }

    // PSX: rsEvent(4, 24, 0, 0) -- SetSFXVol(24)
    rsEvent(4, 24, 0, 0);
    // PSX: rsEvent(5, 0, 0, 0) -- LevelBegin/StopMusic
    rsEvent(5, 0, 0, 0);
    // PSX: rsEvent(12, 7, 0, 0)
    rsEvent(12, 7, 0, 0);

    // PSX: new(348) MoviePlayer -> constructor 0x80014338
    MoviePlayer* player = new MoviePlayer();

    // PSX: SetPath(0x800DB43C = "fe\\movies") then AddPlayMovie(name)
    // MoviePlayer builds full path as "fe\\movies\\<name>"
    char moviePath[128];
    std::snprintf(moviePath, sizeof(moviePath), "fe/movies/%s", name);

    if (!player->Open(moviePath)) {
        LOG("[Game] PlayMovie: cannot open %s, skipping", moviePath);
        delete player;
        goto cleanup;
    }

    // PSX: Play(0x80014534) with skip callback (0x80039330) if skippable
    // PC: blocking frame loop
    {
        f64 prevFrame = Time::GetTimeInSeconds();
        f32 targetDt = 1.0f / player->GetFrameRate();

        while (!player->IsFinished() && !p3d::display->ShouldClose()) {
            f64 now = Time::GetTimeInSeconds();
            f32 elapsed = (f32)(now - prevFrame);
            if (elapsed < targetDt)
                continue;
            prevFrame = now;

            p3d::display->PollEvents();

            // PSX: skip callback checks Start button.
            if (skippable) {
                if (p3d::input) {
                    p3d::input->ServiceInput();
                }
                if (g_actionInput) {
                    g_actionInput->Update(p3d::input);
                }
                if (g_inputManager) {
                    g_inputManager->ServiceHostPads(g_actionInput, true);
                    g_inputManager->Step();
                    if ((g_inputManager->GetControlVal(0) & PsxPad::Start) != 0) {
                        LOG("[Game] PlayMovie: skipped by Start");
                        break;
                    }
                }
            }

            player->AdvanceFrame();

            g_display->BeginFrame();
            player->Render();
            g_display->EndFrame();
        }
    }

    // PSX: destructor 0x8001434C with param 3
    delete player;

cleanup:
    // PSX: if (unloadLevel) { LoadOverlay(0); ReloadFont("xc/fonts.1"); LoadCharTexture(0); }
    if (unloadLevel) {
        if (g_oxFontFile) {
            g_oxFontFile->ReloadFont("XC/FONTS.1");
        }
        if (g_characterManager) {
            g_characterManager->LoadCharTexture((u32)AITypes::TT_PLAYER);
        }
    }

    // PSX: rsEvent(13, 7, 0, 0) -- cleanup
    rsEvent(13, 7, 0, 0);
    // PSX: rsEvent(6, 0, 0, 0) -- StopMusic
    rsEvent(6, 0, 0, 0);
    // PSX: rsEvent(3, 0, 0, 0) -- UnloadLevel sound
    rsEvent(3, 0, 0, 0);
}

// PSX: FreeXconFE__4Game (GAME.CPP:3812, 0x8002C7A4)
// Destroys feMenuMgr, gameMenu, and HUD via virtual destructor.
void Game::FreeXconFE() {
    MARKFUNCTION(0x8002C7A4);
#if CUSTOM_MENU
    if (g_feCustomMenuMgr) {
        g_feCustomMenuMgr->Shutdown();
        delete g_feCustomMenuMgr;
        g_feCustomMenuMgr = nullptr;
    }
#endif

    if (g_feMenuMgr) {
        delete g_feMenuMgr;
        g_feMenuMgr = nullptr;
    }
    if (g_gameMenu) {
        delete g_gameMenu;
        g_gameMenu = nullptr;
    }
    if (g_hud) {
        if (g_hud->displayHandler) {
            g_hud->displayHandler->RemoveFromList();
            delete g_hud->displayHandler;
            g_hud->displayHandler = nullptr;
        }
        delete g_hud;
        g_hud = nullptr;
    }
}

// PSX: InitXconFSImage__4Game (GAME.CPP:3826, 0x8002C838)
void Game::InitXconFSImage() {
    MARKFUNCTION(0x8002C838);
    // PSX: sets up VRAM cell/palette areas for fullscreen images,
    // reloads font from "xc/fonts.1", creates CFrontEndSound (gp+72)
    if (!g_oxFontFile) {
        g_oxFontFile = new oxFontFile();
    }
    g_oxFontFile->ReloadFont("XC/FONTS.1");

    // PSX: new CFrontEndSound stored at gp+72
    if (!g_frontEndSound) {
        g_frontEndSound = new CFrontEndSound();
    }
}

// PSX: FreeXconFSImage__4Game (GAME.CPP:3859, 0x8002C998)
void Game::FreeXconFSImage() {
    MARKFUNCTION(0x8002C998);
    // PSX: NOP (empty function)
}

// PSX: LoadXconFE__4Game (GAME.CPP:3789, 0x8002C648)
// Creates feMenuMgr, gameMenu, and HUD. Sets up VRAM, loads overlay 1.
void Game::LoadXconFE() {
    MARKFUNCTION(0x8002C648);
    if (g_feMenuMgr) return; // already loaded

    // PSX: DeleteAllocators(cellAlligator)
    // PSX: InitCellArea({960,64,64,56}), InitPal4Area({960,120,64,4}), InitPal8Area({960,124,64,4})
    // PSX: LoadOverlay(1) - loads FE overlay

    // PSX: ReloadFont(gp[56], "xc/fonts.1")
    if (!g_oxFontFile) {
        g_oxFontFile = new oxFontFile();
    }
    g_oxFontFile->ReloadFont("XC/FONTS.1");

    // PSX: new(100) feMenuMgr -> gp[48]
    g_feMenuMgr = new feMenuMgr();

    // PSX: new(92) gameMenu -> gp[52]
    g_gameMenu = new gameMenu();

    // PSX: new(712) HUD -> g_hud
    g_hud = new HUD();

    // PSX: HUD constructor registers DisplayXHUD handler at pri -40 in handlerSet2
    // On PC, we add it after construction since our Handler is heap-allocated.
    g_hud->displayHandler = g_game->GetHandlerSet2().AddHandler([](Handler*) {
        if (g_hud) g_hud->Display();
    }, -40);

    // PSX: feMenuMgr->Init("xc/fe.1", gp[56])  -- gp[56] is oxFontFile
    g_feMenuMgr->Init("XC/FE.1", g_oxFontFile);

    // PSX: gameMenu->Init("xc/gamemenu.1", gp[56])
    g_gameMenu->Init("XC/GAMEMENU.1", g_oxFontFile);

    // PSX: hud->Init("xc/hud.1", gp[56])
    g_hud->Init("XC/HUD.1", g_oxFontFile);

#if CUSTOM_MENU
    if (!g_feCustomMenuMgr) {
        g_feCustomMenuMgr = new feCustomMenuMgr();
        g_feCustomMenuMgr->Init(&g_customText);
    }
#endif
}

// PSX: fade globals (gp+3388, gp+3392)
u8 Game::s_fadeStep = 17;
u8 Game::s_fadeCounter = 0;

static f32 s_fadeAccumSec = 0.0f;
static f64 s_fadeLastTimeSec = -1.0;

// PSX: FadeBegin__4Game (GAME.CPP:3869, 0x8002C9A0)
void Game::FadeBegin() {
    MARKFUNCTION(0x8002C9A0);
    // PSX: fadeStep = 17, fadeCounter = 0
    s_fadeStep = 17;
    s_fadeCounter = 0;
    s_fadeAccumSec = 0.0f;
    s_fadeLastTimeSec = -1.0;
}

// PSX: FadeEnd__4Game (GAME.CPP:3875, 0x8002C9B4)
void Game::FadeEnd() {
    MARKFUNCTION(0x8002C9B4);
    // PSX: NOP (empty function)
}

// PSX: FadeUpdate__4Game (GAME.CPP:3879, 0x8002C9BC)
// Returns 1 if fade still in progress, 0 when complete (counter >= 255).
s32 Game::FadeUpdate() {
    MARKFUNCTION(0x8002C9BC);

    constexpr f32 kFadeFrameSec = 1.0f / 30.0f;

    const f64 now = Time::GetTimeInSeconds();
    f32 dt = kFadeFrameSec;
    if (s_fadeLastTimeSec >= 0.0) {
        dt = (f32)(now - s_fadeLastTimeSec);
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.25f) dt = 0.25f;
    }
    s_fadeLastTimeSec = now;

    s_fadeAccumSec += dt;
    while (s_fadeAccumSec >= kFadeFrameSec && s_fadeCounter < 255) {
        s_fadeAccumSec -= kFadeFrameSec;

        // PSX: fadeCounter += fadeStep; clamp to 255
        const s32 newVal = (s32)s_fadeCounter + (s32)s_fadeStep;
        s_fadeCounter = (newVal < 255) ? (u8)newVal : 255;
    }

    return (s_fadeCounter < 255) ? 1 : 0;
}

// PSX: FadeRender__4Game (GAME.CPP:3893, 0x8002C9F8)
// Draws a fullscreen black POLY_F4 with alpha=fadeCounter at layer 5.
void Game::FadeRender() {
    MARKFUNCTION(0x8002C9F8);
    // PSX: EnterLayer(view, 5), setup POLY_F4 (512x240),
    // set RGB to fadeCounter, render primitive, ExitLayer(view, 5)
    // PC: draw a fullscreen colored quad with alpha blending
    ScreenDraw::DrawColoredQuad(0, 0, 0, s_fadeCounter);
}

#if CUSTOM_MENU
// PC: fades gameplay in from black (reuses the same FadeUpdate pacing as
// FadeRender, just inverted
void Game::PlayFadeInHandlerCB(Handler*) {
    if (!g_game || !g_game->playFadeInActive) {
        return;
    }

    const s32 stillFading = FadeUpdate();

    // Start the music partway through the reveal (halfway) rather than
    // waiting for the screen to be fully visible, so it doesn't feel like
    // it's trailing behind the picture. s_fadeCounter runs 0..255.
    if (g_deferLevelBeginMusic && s_fadeCounter >= 128) {
        g_deferLevelBeginMusic = false;
        // The track was already decoded by RS_LEVEL_BEGIN's deferred path
        // (Sound::PreloadMusicTrack) back during the loading screen, so
        // this is just a cheap voice start - no file I/O/decode here, so it
        // can't stall the fade.
        if (g_sound) {
            g_sound->StartPreloadedMusic();
        }
    }

    if (!stillFading) {
        g_game->playFadeInActive = false;
        return;
    }

    ScreenDraw::DrawColoredQuad(0, 0, 0, (u8)(255 - s_fadeCounter));
}
#endif

void DrawDebugInfo() {
#ifdef DEBUG
    if (g_textManager) {
        g_textManager->SetFontByName("Legal");
        g_textManager->SetScale(SCREEN_SCALE_Y(0.15f), SCREEN_SCALE_Y(0.15f));
        g_textManager->SetAlignment(TextAlign_Left);
        g_textManager->SetWrapWidth(0.0f);
        g_textManager->SetLineSpacing(0);
        g_textManager->SetPromptsEnabled(false);
        g_textManager->SetShadow(false);
        g_textManager->SetOutline(true);
        g_textManager->SetColor(255, 255, 0);

        char ver[64];
        std::snprintf(ver, sizeof(ver), "RECHAN DEBUG BUILD %s", GAME_VERSION);

        g_textManager->PrintString(ver,
                                   HudX(0.0f),
                                   HudY(0.0f));


        if (!g_drawDebugConsole)
            return;

        if (g_textManager && g_time) {
            g_textManager->SetFontByName("Legal");
            g_textManager->SetScale(SCREEN_SCALE_Y(0.108f), SCREEN_SCALE_Y(0.108f));
            g_textManager->SetAlignment(TextAlign_Right);
            g_textManager->SetWrapWidth(0.0f);
            g_textManager->SetLineSpacing(0);
            g_textManager->SetPromptsEnabled(false);
            g_textManager->SetShadow(false);
            g_textManager->SetOutline(true);
            g_textManager->SetColor(0, 255, 0);

            char perf[128];
            std::snprintf(perf, sizeof(perf), "%.1f fps | logic %.2fms  draw %.2fms  swap %.2fms",
                          g_time->fps, g_frameProfile.logicMs, g_frameProfile.drawSubmitMs, g_frameProfile.swapMs);
            g_textManager->PrintString(perf, SCREEN_WIDTH - HudX(0.0f), HudY(0.0f));
        }

        g_textManager->SetScale(SCREEN_SCALE_Y(0.108f), SCREEN_SCALE_Y(0.108f));
        g_textManager->SetAlignment(TextAlign_Left);
        g_textManager->SetColor(128, 128, 128);

        s32 i = 0;
        for (auto& it : Log::Get().GetMessageStack()) {
            g_textManager->PrintString(it.c_str(),
                                       HudX(0.0f),
                                       HudY(32.0f + 4.0f * i));
            i++;
        }
    }
#endif
}
