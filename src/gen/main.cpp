#include "common.h"
#include "p3d/context.h"
#include "p3d/fileio.h"
#include "p3d/input.h"
#include "p3d/inventory.h"
#include "p3d/loadmanager.h"
#include "p3d/texture.h"
#include "p3d/shader.h"
#include "p3d/stream.h"
#include "p3d/skeleton.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include "pc/audio.h"
#include "pc/crashreporter.h"
#include "pc/debugui.h"
#include "pc/inputaction.h"
#include "pc/rechan_icon_embedded.h"
#include "pc/textmgr.h"
#include "gen/game.h"
#include "gen/world.h"
#include "gen/time.h"
#include "gen/display.h"
#include "gen/geometry.h"
#include "snd/rsevent.h"
#include "extra/fecustommenumgr.h"
#if !defined(RC_PLATFORM_PSP)
#include "extra/assetexporter.h"
#endif
#include "pc/apppaths.h"
#if MOD_LOADER
#include "extra/gltfloader.h"
#endif
#include "gen/model.h"
#include "radlib/rtask.h"
#include "ai/player.h"
#if MOD_LOADER
#include "extra/modloader.h"
#endif

#if AUTO_UPDATER
#include "extra/autoupdater.h"
#endif

#include "extra/version.h"

#include <vector>
#include <algorithm>

#if defined(RC_PLATFORM_SWITCH)
#include <unistd.h>
#include <switch.h>
#include "pddi/gles/glesrender.h"
#endif

#if defined(RC_PLATFORM_PSP)
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include "psp/psp_perf.h"
namespace p3d::io { void SetPspDevice(const char* device); }

static void ConfigurePspPerformance() {
    // The native 60 Hz target is CPU-bound while decoding PSX pages and
    // submitting GU geometry.  Request the maximum supported user-mode PSP
    // clock before constructing the game so load-time and frame-time work use
    // the same performance envelope on hardware and PPSSPP.
    const int result = scePowerSetClockFrequency(333, 333, 166);
    if (result < 0) {
        LOG("[PSP] scePowerSetClockFrequency failed: %d", result);
    }
}
#endif

static f32 sSavedMasterVolume = 1.0f;
static bool sWindowedCaptureRequestedByClick = false;

static void TrySetPlatformWindowIcon() {
    if (!p3d::display) {
        return;
    }

    p3d::display->SetIcon(kRechanIconWidth, kRechanIconHeight, kRechanIconRgba);
}

static bool OnWndProc(const pddiWndMessage& msg) {
    switch (msg.event) {
        case PDDI_WND_FOCUS:
            if (msg.param1) {
                // Regained focus
                if (p3d::input)
                    p3d::input->SetEnabled(true);
                //if (AudioEngine::IsInitialized())
                //    AudioEngine::SetMasterVolume(sSavedMasterVolume);
                // Keep the mouse free on focus gain in windowed mode so a titlebar
                // click/drag can move the window. In-content mouse clicks will
                // recapture below via PDDI_WND_MOUSEBUTTON.
                if (g_display) {
                    if (DebugUI::IsEnabled()) {
                        g_display->SetCursorCaptured(false);
                        g_display->SetCursorVisible(true);
                    }
                    else if (!g_feCustomMenuMgr || !g_feCustomMenuMgr->IsActive()) {
                        if (g_display->GetScreenMode() == ScreenMode_Windowed) {
                            if (sWindowedCaptureRequestedByClick) {
                                g_display->SetCursorCaptured(true);
                            }
                            else {
                                g_display->SetCursorCaptured(false);
                                g_display->SetCursorVisible(true);
                            }
                            sWindowedCaptureRequestedByClick = false;
                        }
                        else {
                            g_display->SetCursorCaptured(true);
                        }
                    }
                }
            }
            else {
                // Lost focus
                if (p3d::input)
                    p3d::input->SetEnabled(false);
                //if (AudioEngine::IsInitialized()) {
                //    sSavedMasterVolume = AudioEngine::GetMasterVolume();
                //    AudioEngine::SetMasterVolume(0.0f);
                //}
                // Always release mouse when losing focus
                if (g_display)
                    g_display->SetCursorCaptured(false);
                sWindowedCaptureRequestedByClick = false;
            }
            break;
        case PDDI_WND_MOUSEBUTTON:
            // GLFW mouse button callbacks only fire for client-area clicks, not
            // non-client titlebar drags. Use that to defer recapture in windowed
            // mode until the user clicks back into game content.
            if (msg.param2 && g_display &&
                g_display->GetScreenMode() == ScreenMode_Windowed &&
                !DebugUI::IsEnabled() &&
                (!g_feCustomMenuMgr || !g_feCustomMenuMgr->IsActive())) {
                sWindowedCaptureRequestedByClick = true;
                g_display->SetCursorCaptured(true);
            }
            break;
        case PDDI_WND_SCROLL:
            if (g_actionInput && g_feCustomMenuMgr && g_feCustomMenuMgr->IsActive()) {
                const s32 dir = (msg.fparam2 > 0.0) ? 1 : -1;
                g_actionInput->AddScrollDelta(dir);
            }
            break;
        default:
            break;
    }
    return false;
}

int main(int argc, char** argv) {
#if !defined(NDEBUG) && !defined(RC_PLATFORM_PSP)
    Log::Get().Init();
#endif
    CrashReporter::Install();

#if defined(RC_PLATFORM_SWITCH)
    p3d::io::CreateDirectories("sdmc:/switch/rechan");
    chdir("sdmc:/switch/rechan");

    // Boosted handheld preset
    apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x92220008);

    const AppletType appletType = appletGetAppletType();
    if (appletType != AppletType_Application && appletType != AppletType_SystemApplication) {
        consoleInit(NULL);
        printf("[rechan] Launched in applet mode (type %d).\n", (int)appletType);
        printf("[rechan] ReChan needs to run as a full application, not an applet.\n");
        printf("[rechan] Press PLUS to exit.\n");
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
        return 1;
    }
#endif

    if (argc >= 2 && std::strcmp(argv[1], "--test-crash-reporter") == 0) {
        CrashReporter::TriggerTestCrash();
    }

    // Headless verification/export path used by automated round-trip checks
    // and mod authors who want the same output as the DebugUI button.
#if !defined(RC_PLATFORM_PSP)
    if (argc >= 2 && std::strcmp(argv[1], "--export-assets") == 0) {
        const char* outputDir = (argc >= 3 && argv[2] && argv[2][0])
            ? argv[2]
            : apppaths::kExportedAssetsDefaultDir;
        AssetExporter& exporter = AssetExporter::Instance();
        exporter.BuildCatalog();
        const int exported = exporter.ExportAllCategories(outputDir);
        LOG("[AssetExporter] Headless export completed: %d asset bundle(s)", exported);
#if !defined(NDEBUG)
        Log::Get().Shutdown();
#endif
        return exported > 0 ? 0 : 2;
    }
#endif

#if MOD_LOADER
    if (argc >= 3 && std::strcmp(argv[1], "--verify-png-baseline") == 0) {
        const bool unchanged = ModLoader::Instance().IsUnmodifiedTextureDump(argv[2]);
        LOG("[ModLoader] PNG baseline %s: %s", unchanged ? "matched" : "not matched", argv[2]);
#if !defined(NDEBUG)
        Log::Get().Shutdown();
#endif
        return unchanged ? 0 : 5;
    }
#endif

#if defined(RC_PLATFORM_PSP)
    ConfigurePspPerformance();
#if defined(PSP_PERF_TELEMETRY)
    PspPerf::Reset();
    PspPerf::Trace("main: telemetry reset");
#endif

    // EBOOT launches with the game directory as the working root on most
    // shells, but explicitly selecting it also makes loading deterministic in
    // PPSSPP and on installations using a different launch directory.
    if (sceIoChdir("ms0:/PSP/GAME/RECHAN/data") < 0) {
        if (sceIoChdir("ef0:/PSP/GAME/RECHAN/data") < 0) {
            sceIoChdir("ms0:/PSP/GAME/RECHAN");
        }
        else {
            p3d::io::SetPspDevice("ef0:");
        }
    }
#if !defined(NDEBUG) || defined(PSP_GU_DIAGNOSTIC) || defined(PSP_STAGE_DIAGNOSTIC)
    Log::Get().Init("PSP_LOG.TXT");
#endif
#endif

    tPlatform* platform = tPlatform::Create();

    tContextInitData init;
    init.xSize = JCS_TARGET_WIDTH;
    init.ySize = JCS_TARGET_HEIGHT;
    init.title = JCS_TITLE;

    tContext* ctx = platform->CreateContext(init);
    if (!ctx) {
#if defined(RC_PLATFORM_SWITCH)
        consoleInit(NULL);
        printf("[rechan] CreateContext() FAILED: %s\n", GetLastGlesInitError());
        printf("[rechan] Press PLUS to exit.\n");
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
#endif
        return 1;
    }

    TrySetPlatformWindowIcon();

#if MOD_LOADER
    if (argc >= 3 && std::strcmp(argv[1], "--verify-glb") == 0) {
        OriginalGeo* geometry = GLTFLoader::LoadGeo(argv[2]);
        const bool valid = geometry && geometry->meshBuffer;
        delete geometry;
        platform->DestroyContext(ctx);
        tPlatform::Destroy();
        LOG("[GLTFLoader] Verification %s: %s", valid ? "passed" : "failed", argv[2]);
#if !defined(NDEBUG)
        Log::Get().Shutdown();
#endif
        return valid ? 0 : 3;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--verify-stree") == 0) {
        OriginalSTree* character = GLTFLoader::LoadSTree(argv[2]);
        const bool valid = character && character->skeleton && character->skinData
            && character->skeleton->numJoints > 0
            && character->skeleton->joints[0].meshBuffer;
        delete character;
        platform->DestroyContext(ctx);
        tPlatform::Destroy();
        LOG("[GLTFLoader] Skinned verification %s: %s", valid ? "passed" : "failed", argv[2]);
#if !defined(NDEBUG)
        Log::Get().Shutdown();
#endif
        return valid ? 0 : 4;
    }
#endif

    p3d::display->SetWndProc(OnWndProc);
    p3d::display->AddOverlayCallback(DebugUI::Draw);
    p3d::display->AddOverlayCallback(DrawDebugInfo);

    rTaskInit(100);
    rInitTaskList(&rMainTaskList);
    rInitTaskList(&rFrameTaskList);
    rFrameCount = 0;
    rFrameCount60 = 0;

    Game game;
    if (!g_actionInput) {
        g_actionInput = new ActionInput();
    }
    if (!g_textManager) {
        g_textManager = new TextManager();
        g_textManager->Init();
        TextFontDesc desc = {};
        desc.name = "Menu";
        desc.path = "pc/fonts/YIKES!__.TTF";
        desc.pixelHeight = 48;
        if (!g_textManager->LoadFont(desc)) {
            LOG("[TextManager] Failed to load menu font: %s", desc.path);
        }

        desc.name = "Legal";
        desc.path = "pc/fonts/Roboto-Regular.ttf";
        desc.pixelHeight = 48;
        if (!g_textManager->LoadFont(desc)) {
            LOG("[TextManager] Failed to load menu font: %s", desc.path);
        }
    }

    game.Open();

#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("main: game open");
#endif

#if defined(RC_PLATFORM_PSP) && defined(PSP_AUTO_START_PHASE1)
    // The USB QA EBOOT starts from a deterministic playable checkpoint.  This
    // is intentionally independent of PSP_PERF_SOAK: soak keeps the session
    // alive, while this flag selects the scene used by the benchmark.  The
    // shipping build does not define it and retains the normal boot flow.
    // The level table is loaded by Game::gsInitState, which runs after this
    // bootstrap call. Keep the sentinel/index simple here and resolve the
    // stable phase ID in gsOpenFEState once GAME_LN.TXT is available.
    g_selectedLevel = 0;
    game.QueueTitleNewGameStart();
#if defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("main: queued phase1 auto start");
#endif
#endif

#if MOD_LOADER
    ModLoader::Instance().Init();
#endif

    game.SetState(GameState::Intro);

    p3d::context->SetClearColour(pddiColour(30, 30, 35));

    MARKFUNCTION(0x8002635C);

#if defined(RC_PLATFORM_PSP)
    // Frame timing in integer microseconds: the same clock that
    // Time::GetTimeInSeconds() divides by 1e6, without the soft-float double
    // conversions/subtractions the Allegrex would pay every frame.
    u64 prevTimeUs = sceKernelGetSystemTimeWide();
#else
    f64 prevTime = Time::GetTimeInSeconds();
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    u32 pspPlayTraceFrames = 0;
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_STAGE_DIAGNOSTIC)
    GameState pspLastObservedState = static_cast<GameState>(-1);
    u32 pspStateTransitionProbe = 0;
#endif

    while (!p3d::display->ShouldClose()) {
#if defined(RC_PLATFORM_PSP)
        const u64 frameStartUs = sceKernelGetSystemTimeWide();
        // Exact integer delta, one correctly rounded single-precision divide.
        f32 realDt = static_cast<f32>(static_cast<s32>(frameStartUs - prevTimeUs)) / 1000000.0f;
        prevTimeUs = frameStartUs;
#else
        f64 frameStart = Time::GetTimeInSeconds();

        f32 realDt = (f32)(frameStart - prevTime);
        prevTime = frameStart;
#endif
        g_time->Tick(realDt);
#if defined(RC_PLATFORM_PSP) && defined(PSP_AUTO_START_PHASE1)
        DebugUI::BeginBenchmarkFrame(&game);
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_STAGE_DIAGNOSTIC)
        if (game.GetState() != pspLastObservedState && pspStateTransitionProbe < 24u) {
            Log::Get().LogMessage("[PSP State] transition #%lu state=%d prev=%d",
                static_cast<unsigned long>(pspStateTransitionProbe++),
                static_cast<int>(game.GetState()),
                static_cast<int>(game.GetPrevState()));
            pspLastObservedState = game.GetState();
        }
#endif
#if HIGH_FPS_PLAY_PRESENTATION
        const GameState curState = game.GetState();
        if (curState == GameState::EndLevelLoop) {
            const s32 fixedLogicSteps = g_time->BeginPlayFixedStep();
            for (s32 i = 0; i < fixedLogicSteps; ++i) {
                g_time->Step();
            }
        }
        else if (curState != GameState::Play) {
            g_time->Step();
        }
#else
        g_time->Step();
#endif

        ++rFrameCount;
        ++rFrameCount60;
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        static u32 pspTaskProbeFrames = 0;
        const bool probePspTasks = game.GetState() == GameState::Play && pspTaskProbeFrames < 4;
        if (probePspTasks) {
            LOG("[PSP Frame] before frame tasks frame=%lu", static_cast<unsigned long>(rFrameCount));
        }
#endif
        rDoTaskList(&rFrameTaskList, static_cast<u32>(rFrameCount));
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspTasks) {
            LOG("[PSP Frame] after frame tasks frame=%lu", static_cast<unsigned long>(rFrameCount));
            ++pspTaskProbeFrames;
        }
#endif

        p3d::display->PollEvents();

        if (p3d::input) {
            p3d::input->ServiceInput();
        }
        if (g_actionInput) {
            g_actionInput->Update(p3d::input);
        }
#if defined(__SWITCH__)
        if (p3d::input &&
            p3d::input->IsGamepadButtonDown(GpBtn::LB) &&
            p3d::input->IsGamepadButtonDown(GpBtn::RB) &&
            p3d::input->IsGamepadButtonTriggered(GpBtn::Back)) {
            g_drawDebugConsole = !g_drawDebugConsole;
        }
#endif
        if (g_inputManager) {
            const ActionInput* actionInputForGame = DebugUI::ShouldBlockGameInput()
                ? nullptr
                : g_actionInput;
            bool commitInputNow = true;
#if HIGH_FPS_PLAY_PRESENTATION
            if (game.GetState() == GameState::Play) {
                commitInputNow = false;
            }
#endif
            g_inputManager->ServiceHostPads(actionInputForGame, commitInputNow);
        }

        const GameState stateBeforeStep = game.GetState();
#if defined(RC_PLATFORM_PSP)
        const u64 stepT0Us = sceKernelGetSystemTimeWide();
#else
        const f64 stepT0 = Time::GetTimeInSeconds();
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_STAGE_DIAGNOSTIC)
        static u32 pspStageMainPlayFrames = 0;
        const bool pspStageMainProbe = stateBeforeStep == GameState::Play
            && pspStageMainPlayFrames < 8u;
        if (pspStageMainProbe) {
            Log::Get().LogMessage("[PSP Main Stage] before step frame=%lu play=%lu",
                static_cast<unsigned long>(rFrameCount),
                static_cast<unsigned long>(pspStageMainPlayFrames));
        }
#endif
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        static u32 pspPlayStepProbeFrames = 0;
        const bool probePspPlayStep = stateBeforeStep == GameState::Play
            && pspPlayStepProbeFrames < 4;
        if (probePspPlayStep) {
            LOG("[PSP Frame] before game step frame=%lu", static_cast<unsigned long>(rFrameCount));
        }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        const bool tracePspPlayFrame = stateBeforeStep == GameState::Play && pspPlayTraceFrames < 3;
        if (tracePspPlayFrame) {
            PspPerf::Trace("play: before Step");
        }
#endif
        g_frameProfile.drawSubmitMs = 0.0f;
        g_frameProfile.swapMs = 0.0f;
        bool running = game.Step();
#if defined(RC_PLATFORM_PSP) && defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageMainProbe) {
            Log::Get().LogMessage("[PSP Main Stage] after step frame=%lu state=%d running=%u",
                static_cast<unsigned long>(rFrameCount),
                static_cast<int>(game.GetState()), running ? 1u : 0u);
        }
#endif
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspPlayStep) {
            LOG("[PSP Frame] after game step frame=%lu state=%d running=%u",
                static_cast<unsigned long>(rFrameCount),
                static_cast<int>(game.GetState()), running ? 1u : 0u);
        }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        if (tracePspPlayFrame) {
            PspPerf::Trace("play: after Step");
        }
#endif
#if defined(RC_PLATFORM_PSP)
        const f32 stepTotalMs = static_cast<f32>(static_cast<s32>(sceKernelGetSystemTimeWide() - stepT0Us)) / 1000.0f;
#else
        const f32 stepTotalMs = (f32)((Time::GetTimeInSeconds() - stepT0) * 1000.0);
#endif
        g_frameProfile.logicMs = stepTotalMs
            - g_frameProfile.drawSubmitMs
            - g_frameProfile.swapMs;
        if (g_frameProfile.logicMs < 0.0f) {
            // Timer granularity can make the subtraction round a tiny amount
            // below zero on a short frame; never publish a negative phase.
            g_frameProfile.logicMs = 0.0f;
        }
        if (!running) {
            // PC: If the game loop signals to stop, break out of the loop to shut down.
            if (game.GetState() == GameState::End) {
                break;
            }

            if (stateBeforeStep == GameState::DetermineGameOverState) {
                game.SetState(GameState::DetermineNextGameState);
            }
            else {
                // PSX: SetLivesLeft(g_player, savedLives)
                Player::s_player->SetLivesLeft(4);
                game.SetState(GameState::QueueLevelLoad);
            }
        }
        else {
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
            if (probePspPlayStep) {
                LOG("[PSP Frame] before main tasks frame=%lu", static_cast<unsigned long>(rFrameCount));
            }
#endif
            rDoTaskList(&rMainTaskList, static_cast<u32>(rFrameCount60));
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
            if (probePspPlayStep) {
                LOG("[PSP Frame] after main tasks frame=%lu", static_cast<unsigned long>(rFrameCount));
            }
#endif
        }
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        if (tracePspPlayFrame) {
            PspPerf::Trace("play: after main tasks");
        }
#endif

        jcsUpdateAmbience();
        jcsUpdateDialogCD();

#if defined(RC_PLATFORM_PSP)
        // A no-op on PSP (SwapBuffers already waits for vblank).
        g_time->WaitForFrameEnd(0.0);
#else
        g_time->WaitForFrameEnd(frameStart);
#endif
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspPlayStep) {
            LOG("[PSP Frame] after wait frame=%lu", static_cast<unsigned long>(rFrameCount));
            ++pspPlayStepProbeFrames;
        }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        if (tracePspPlayFrame) {
            PspPerf::Trace("play: after WaitForFrameEnd");
        }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageMainProbe) {
            Log::Get().LogMessage("[PSP Main Stage] after wait frame=%lu",
                static_cast<unsigned long>(rFrameCount));
        }
#endif
#if defined(RC_PLATFORM_PSP)
#if defined(PSP_PERF_TELEMETRY)
        // Profile only the interactive game state. Boot, title and loading
        // have intentionally different costs and would hide the gameplay
        // frame budget. The PSP benchmark freezes each 600-frame scenario
        // before moving to the next level; it never renders a measurement HUD.
        if (game.GetState() == GameState::Play && !DebugUI::IsBenchmarkFinished()) {
            PspPerf::RecordFrame(
                static_cast<u32>(sceKernelGetSystemTimeWide() - frameStartUs),
                g_frameProfile.logicMs,
                g_frameProfile.drawSubmitMs,
                g_frameProfile.swapMs);
#if defined(PSP_AUTO_START_PHASE1)
            DebugUI::OnBenchmarkFrameRecorded();
            const bool queuedNextBenchmarkScenario =
                DebugUI::QueueNextBenchmarkScenario(&game);
#else
            constexpr bool queuedNextBenchmarkScenario = false;
#endif
            // A non-soak profile may still use the multi-scenario runner. Do
            // not stop after a completed scenario when the next load was
            // queued; stop only after the final report.
            if (PspPerf::ShouldStop() && !queuedNextBenchmarkScenario) {
                break;
            }
#if defined(PSP_STAGE_DIAGNOSTIC)
            if (pspStageMainProbe) {
                Log::Get().LogMessage("[PSP Main Stage] after profile frame=%lu",
                    static_cast<unsigned long>(rFrameCount));
                ++pspStageMainPlayFrames;
            }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
            if (tracePspPlayFrame) {
                PspPerf::Trace("play: after RecordFrame");
                ++pspPlayTraceFrames;
            }
#endif
        }
#endif
#endif

#if !defined(RC_PLATFORM_PSP)
        // The PSP has no window title (pspDisplay::SetTitle is empty), so it
        // skips this once-per-second double-formatting snprintf.
        static f32 titleAccumSec = 0.0f;
        titleAccumSec += g_time->deltaTime;
        if (titleAccumSec >= 1.0f) {
            char titleBuf[128];
            snprintf(titleBuf, sizeof(titleBuf), "%s - %.1f fps (%.2f ms)", JCS_TITLE, g_time->fps, g_time->deltaTime * 1000.0f);
            p3d::display->SetTitle(titleBuf);
            titleAccumSec = 0.0f;
        }
#endif
    }

#if defined(RC_PLATFORM_PSP)
#if defined(PSP_PERF_TELEMETRY)
    PspPerf::WriteReport();
    PspPerf::Trace("main: report written");
#endif
    ShutdownPspGeometryFrameScratch();
#endif
    platform->DestroyContext(ctx);
    tPlatform::Destroy();

    if (g_textManager) {
        g_textManager->Shutdown();
        delete g_textManager;
        g_textManager = nullptr;
    }

#if AUTO_UPDATER
    if (g_autoUpdater) {
        g_autoUpdater->Shutdown();
        delete g_autoUpdater;
        g_autoUpdater = nullptr;
    }
#endif

    if (g_psxDiscExtractor) {
        g_psxDiscExtractor->Shutdown();
        delete g_psxDiscExtractor;
        g_psxDiscExtractor = nullptr;
    }

    delete g_actionInput;
    g_actionInput = nullptr;

#if MOD_LOADER
    ModLoader::Instance().Shutdown();
#endif

    LOG("Clean shutdown");
#if !defined(NDEBUG) || defined(PSP_GU_DIAGNOSTIC)
    Log::Get().Shutdown();
#endif

    return 0;
}
