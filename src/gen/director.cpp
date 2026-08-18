#include "common.h"
#include "gen/director.h"
#include "gen/ai.h"
#include "gen/animmat.h"
#include "gen/cammgr.h"
#include "gen/effects.h"
#include "gen/geffect.h"
#include "gen/deadpool.h"
#include "gen/model.h"
#include "gen/weffect.h"
#include "ai/behaviour.h"
#include "ai/destroy.h"
#include "ai/obstacle.h"
#include "ai/humanoid.h"
#include "ai/player.h"
#include "gen/charmgr.h"
#include "gen/game.h"
#include "gen/display.h"
#include "gen/camera.h"
#include "gen/animstruct.h"
#include "gen/scoremgr.h"
#include "gen/time.h"
#include "gen/world.h"
#include "gen/colsect.h"
#include "fe/hud.h"
#include "snd/drctrsnd.h"
#include "snd/snddrct.h"
#include "snd/sound.h"
#include "p3d/context.h"
#include "p3d/inventory.h"
#include "p3d/p3dmath.h"
#include "p3d/ramtexanim.h"
#include "snd/rsevent.h"
#include "pc/tim.h"
#include "pc/log.h"
#include "pc/inputaction.h"
#include "ai/door.h"
#include "ai/ladder.h"
#include "gen/director_scripts_800d6f14.inc"
#include "gen/director_scripts_overlay7_8001fa3c.inc"

// PSX globals used by DIRECTOR.CPP script control flow.
// - g_directorActive (defined in GAME.CPP): gp+20 symbol `directorTimeOut` (0x800DC960)
//   used by gsPlayState to run Director-only frames.
// - returnUnderflowScriptWord mirrors the gp+0x41C Return-underflow target address.
// - returnAddressIndex mirrors gp+0x420 used by Call/Return.
// - returnAddress mirrors PSX returnAddress[] storage.
// - codeSnipThing: gp+869 (stored in global, NOT in Director object)
// PSX data at 0x800DCD68 initializes this script word to opcode 2 (EndScript).
static s32 returnUnderflowScriptWord = 2;
static s32 directorElapsedFrames = 0;
static s32 returnAddressIndex = 0;
static s32 directorDialogCounter = -1;
static s32 directorDialogLimit = 180;
static s32* returnAddress[8] = {};
static Thing* g_codeSnipThing = nullptr; // PSX: gp[869]
// PSX: gp+3476 = gp[869] - same as g_codeSnipThing! ProcessDoorFunc/LadderFunc read
// the door/ladder from this global. SetCodeSnip stores the thing here via gp[869].
static Thing*& g_selectedDoorOrLadder = g_codeSnipThing;
static s32 g_dialogHandle = 0;          // PSX: gp[3496/4=874]
static s32 g_checkpointDataValue = 0;   // PSX: dword_800D6F90
static s32 g_checkpointBlockIndex = 0;  // PSX: gp+0xDA0 (source for opcodes 0x7B/0x7D)
static u8 g_checkpointPetalIndex = 0;   // PSX: gp+0xDA4 (0='a', 1='b', ...)

enum class DirectorScriptState : s32 {
    Idle = 0,
    SetScript = 534,
    SetCodeSnip = 537,
};

static constexpr s32 ToScriptStateValue(DirectorScriptState state) {
    return static_cast<s32>(state);
}

static constexpr s32 kHashJackie = 0x0507A1F5; // p3dHash("Jackie")
static constexpr s32 kHashDante = 0x004A85A5;  // p3dHash("Dante")
static constexpr s32 kHashChef = 0x00049EB6;   // p3dHash("Chef")
static constexpr s32 kHashClown = 0x004A36DE;  // p3dHash("Clown")
static constexpr s32 kHashBarney = 0x048894C9; // p3dHash("Barney")
static constexpr s32 kHashDisco = 0x004B099F;  // p3dHash("Disco")

// PSX DIRECTOR.CPP: ProcessCameraFunc case '6' (0x8003D884),
// ProcessLadderFunc case 'B' (0x8003E0D4).
static constexpr s32 kLookAtNisPointOffsetXZ = 812;
static constexpr s32 kLookAtNisPointOffsetY = 108;
static constexpr s32 kLadderFacePointOffset = 1024;

static constexpr u32 kDirectorMatrixEffectHash = 0x004CC954u;
static constexpr u32 kDirectorAttackEffectHash = 0x03E24164u;
static constexpr u32 kDirectorPosBEffectHash = 0x0B0E21C4u;
static constexpr u32 kDirectorNisEffectHash = 0x0FF877C4u;
static constexpr u32 kDirectorFallbackCheckpointHash = 0x0E6B6563u;
static constexpr u32 kDirectorOverlay7ScriptBase = 0x8001FA3Cu;
static constexpr u32 kDirectorOverlay7ScriptEndExclusive = 0x80020A08u;
static constexpr u32 kDirectorScriptIntroAllDante = 0x8001FA3Cu;
static constexpr u32 kDirectorScriptIntroChef = 0x8001FA54u;
static constexpr u32 kDirectorScriptIntroEveryChef = 0x8001FBCCu;
static constexpr u32 kDirectorScriptIntroClown = 0x8001FBFCu;
static constexpr u32 kDirectorScriptIntroEveryClown = 0x8001FDB4u;
static constexpr u32 kDirectorScriptIntroGrontar = 0x8001FDE4u;
static constexpr u32 kDirectorScriptIntroEveryGrontar = 0x8001FF84u;
static constexpr u32 kDirectorScriptIntroDisco = 0x8001FFACu;
static constexpr u32 kDirectorScriptPlayIntroDisco = 0x80020024u;
static constexpr u32 kDirectorScriptIntroEveryDisco = 0x80020140u;
static constexpr u32 kDirectorScriptVictoryChef = 0x80020168u;
static constexpr u32 kDirectorScriptVictoryDisco = 0x800202A8u;
static constexpr u32 kDirectorScriptVictoryGrontar = 0x80020458u;
static constexpr u32 kDirectorScriptVictoryClown = 0x80020588u;
static constexpr u32 kDirectorScriptStartGeneric = 0x800D7048u;
static constexpr u32 kDirectorScriptStartFrantic = 0x800D70B0u;
static constexpr u32 kDirectorScriptGotoPoint = 0x800D803Cu;
static constexpr u32 kDirectorScriptNisLadder1 = 0x800D8058u;
static constexpr u32 kDirectorScriptVictoryPoor = 0x800D8208u;
static constexpr u32 kDirectorScriptVictoryOk = 0x800D8278u;
static constexpr u32 kDirectorScriptVictoryGood = 0x800D82E8u;
static constexpr u32 kDirectorScriptVictoryPerfect = 0x800D8358u;
static constexpr u32 kDirectorScriptDeath = 0x800D83C8u;
static constexpr u32 kDirectorScriptDeathVol = 0x800D8454u;
static constexpr u32 kDirectorScriptDeathFallPavement = 0x800D84E0u;
static constexpr u32 kDirectorScriptDeathFallWater = 0x800D855Cu;
static constexpr u32 kDirectorScriptDeathGeneric = 0x800D8598u;
static constexpr u32 kDirectorScriptDeathFallGoo = 0x800D85C0u;
static constexpr u32 kDirectorScriptNisDoor1 = 0x800D85E4u;
static constexpr u32 kDirectorScriptNisDoor1WithDialog = 0x800D8640u;
static constexpr u32 kDirectorScriptLevelEnd = 0x800D86B0u;
static constexpr u32 kDirectorScriptWaitSubroutine = 0x800D86CCu;
static constexpr u32 kDirectorScriptDefaultBegin = 0x800D86E0u;

void runDirector(Handler* h);
void DrawDirectorOverlays(Handler* h);

static s32 start_generic[] = {
    9, 6, -1, 98, 102, 40, 103, 256, 100, 0,
    99, 255, 5, 14, kHashJackie, 1, 6, 45, 73, 75,
    98, 99, 0, 5, 8, 4
};
// Checkpoint-respawn variant: restores HUD without widescreen bars or timer.
// PSX: the 45-tick start_generic timer ran during the slow CD load, so input was
// ready the moment the screen appeared. PC loads instantly so we skip the wait.
static s32 start_generic_checkpoint[] = {
    73, 75,  // HudFunc(ShowHud) — 73=HudFunc opcode, 75='K'=ShowHud cmd
    4        // Return
};

static s32 start_frantic[] = {
    9, 6, -1, 73, 74, 98, 102, 40, 103, 256,
    100, 0, 99, 255, 5, 14, kHashJackie, 73, 84, kHashJackie,
    85, 5, 15, 0, 346, 16, 0, 346, 1, 21,
    kHashJackie, 346, 17, kHashJackie, 14, kHashJackie, 1, 73, 75, 98,
    99, 0, 5, 8, 22, 0, 346, 4
};

static s32 gotopoint[7] = { 9, 64, 19, kHashJackie, 20, 8, 2 };
static s32 NISladder1[108] = {
    9, 43, 45, 65, 66, 1, 5, 6, -1, 6,
    30, 43, 46, 65, 67, 5, 84, kHashJackie, 88, 0,
    5, 14, kHashJackie, 27, 6, 31, 65, 66, 0, 5,
    6, 58, 65, 68, 5, 6, 83, 65, 69, 5,
    6, 93, 65, 70, 5, 6, 96, 20, 8, 2,
    6, -1, 6, 30, 14, kHashJackie, 1, 84, kHashJackie, 92,
    86, 5, 42, kHashJackie, 122, 169867027, 6, -1, 6, 2,
    43, 44, 6, 3, 43, 54, 169867027, 12, kHashJackie, 6,
    -1, 4, 98, 102, 40, 103, 256, 100, 255, 99,
    255, 104, 2, 5, 6, -1, 4, 17, kHashJackie, 84,
    kHashJackie, 87, 93, 5, 14, kHashJackie, 1, 4
};

static s32 victory_poor[28] = {
    15, 0, 347, 16, 0, 347, 115, 0, 65, 3,
    -2146647188, 3, -2146598624, 117, 21, kHashJackie, 347, 3, -2146598496, 106,
    -2146603316, 118, 73, 76, 1, 3, -2146598436, 4
};
static s32 victory_ok[28] = {
    15, 0, 348, 16, 0, 348, 115, 0, 62, 3,
    -2146647188, 3, -2146598624, 117, 21, kHashJackie, 348, 3, -2146598496, 106,
    -2146603304, 118, 73, 76, 1, 3, -2146598436, 4
};
static s32 victory_good[28] = {
    15, 0, 349, 16, 0, 349, 115, 0, 61, 3,
    -2146647188, 3, -2146598624, 117, 21, kHashJackie, 349, 3, -2146598496, 106,
    -2146603276, 118, 73, 76, 1, 3, -2146598436, 4
};
static s32 victory_perfect[28] = {
    15, 0, 350, 16, 0, 350, 115, 0, 60, 3,
    -2146647188, 3, -2146598624, 117, 21, kHashJackie, 350, 3, -2146598496, 106,
    -2146603264, 118, 73, 76, 1, 3, -2146598436, 4
};
static s32 death[35] = {
    9, 43, 45, 73, 74, 6, -1, 6, 30,
    120, 6, 60, 98, 102, 120, 103, 256, 100,
    0, 99, 255, 101, 9, 5, 107, 109, 6,
    90, 98, 101, 10, 5, 25, -1, 2
};

static s32 death_vol[35] = {
    9, 43, 45, 6, -1, 121, 120, 127, 6,
    -1, 73, 74, 98, 102, 120, 103, 256, 100,
    0, 99, 255, 101, 10, 5, 107, 109, 6,
    30, 98, 101, 10, 5, 25, -1, 2
};
static s32 death_fall_pavement[31] = {
    115, 0, 52, 6, 30, 43, 53, 1, 110, 10, 107, 108, 14, 6, 31, 107,
    108, 13, 6, 32, 107, 108, 12, 107, 108, 22, 117, 118, 6, 60, 4
};
static s32 death_fall_water[15] = {
    115, 0, 53, 6, 15, 107, 108, 25, 6, 30, 117, 118, 6, 60, 4
};
static s32 death_generic[10] = {
    115, 0, 51, 6, 30, 117, 118, 6, 60, 4
};
static s32 death_fall_goo[35] = {
    14, kHashJackie, 69, 3, -2146597480,
    14, kHashJackie, 69, 4, 9,
    56, 62, kHashJackie, 5, 56,
    58, 5, 6, -1, 6,
    15, 56, 59, 5, 6,
    30, 56, 63, 5, 20,
    8, 2, 9, 116, 0
};

// PSX: NISdoor1 at 0x800D85E4 (WORLDPTS.CPP)
// Door enter cutscene: disable input, attach to door, open, wait, teleport, restore
static s32 NISdoor1[23] = {
    9, 56, 62, kHashJackie, 5, 56, 58, 5,
    6, -1, 6, 15, 56, 59, 5, 6,
    30, 56, 63, 5, 20, 8, 2
};

// PSX: NISdoor1WithDialog at 0x800D8640 (WORLDPTS.CPP)
// Same as NISdoor1 but loads and plays dialog first
static s32 NISdoor1WithDialog[28] = {
    9, 116, 0, 109, 99, 56, 62, kHashJackie,
    5, 119, 56, 58, 5, 6, -1, 6,
    15, 56, 59, 5, 6, 30, 56, 63,
    5, 20, 8, 2
};

static s32 levelEnd[7] = { 9, 73, 74, 126, 26, 9, 2 };
static s32 wait_subroutine[5] = { 6, -1, 6, 60, 4 };

// PSX simple one-word/tiny scripts in DIRECTOR globals.
// start_fall/start_door/slot come from 0x800DCD5C..0x800DCD70.
static s32 start_fall[1] = { 2 };
static s32 start_door[1] = { 2 };
static s32 slot[4] = { 2, 2, 0, 0 };

// PSX: globalScripts at 0x800D8734 (SWITCH.CPP)
// Used by gfDirectorVol to resolve a script pointer from an integer index.
static constexpr u32 kGlobalScriptVirtual[] = {
    0x800D86B0u, // levelEnd
    0x800DCD64u, // slot
    0x800DCD64u, // slot
    0x800D83C8u, // death
    0x800DCD64u, // slot
    0x800DCD64u, // slot
    0x800D7170u,
    0x800D7244u,
    0x800D7358u,
    0x800D73F0u,
    0x800D7000u,
    0x800D750Cu,
    0x800D7680u,
    0x800D7784u,
    0x800D7880u,
    0x800D7974u,
    0x800D7A20u,
    0x800D7B3Cu,
    0x800D7C60u,
    0x800D7DA0u,
    0x800D7E48u,
    0x800D7F38u,
    0x800DCD64u, // slot
    0x800DCD64u, // slot
    0x800DCD64u, // slot
    0x800DCD64u, // slot
    0x80020024u, // play_intro_disco
    0x800D803Cu, // gotopoint
    0x800DCD64u, // slot
    0x800DCD64u, // slot
    0x800D70B0u, // start_frantic
    0x800DCD5Cu, // start_fall
    0x800DCD60u, // start_door
    0x800DCD64u, // slot
};

// PSX script tables present in overlay globals.
static s32 defaultBeginScript[21] = {
    9, 98, 100, 255, 5, 10, 30, 8, 2, 124, 25, -1, 4, 123, 25, -1, 4, 125, 25, -1, 4
};

// Intro/victory scripts extracted from BOL_REL.BIN (boss overlay, Region B base 0x8001A758).
// PSX uses Call opcode (3) with raw PSX addresses for subroutine jumps.
// ScriptPtrFromWord resolves these via RegisterScriptRegion.

static s32 intro_all_dante[6] = {
    73, 77, -2147354060, 4, 1717921859, 0
};

static s32 intro_chef[94] = {
    105, 113, 14, kHashJackie, 73, 14, kHashChef, 73, 15, 0,
    359, 16, 0, 359, 43, 50, 360, 18, 15, 17,
    359, 16, 17, 359, 1, 115, 0, 10, 117, 114,
    3, -2147350268, 43, 52, 84, kHashJackie, 89, 90, -1130, 2039,
    27285, 91, 359, 5, 84, kHashChef, 89, 90, -1130, 2039,
    27285, 91, 359, 5, 6, -1, 106, -2147350844, 118, 37,
    0x052EA764, -1130, 2039, 27285, 17, kHashJackie, 43, 51, 14, kHashChef,
    1, 84, kHashJackie, 88, 270, 5, 3, -2147350192, 22, 0,
    359, 22, 17, 359, 73, 77, -2147354036, 15, 0, 357,
    43, 50, 358, 4
};

static s32 intro_every_chef[12] = {
    15, 0, 357, 43, 50, 358, 73, 77, -2147354036, 4, 2003790915, 110
};

static s32 intro_clown[110] = {
    105, 113, 14, kHashJackie, 73, 14, kHashClown, 73, 15, 0,
    363, 43, 50, 364, 16, 0, 363, 18, 15, 23,
    363, 16, 23, 363, 15, 13, 363, 16, 13, 363,
    1, 115, 0, 12, 117, 114, 3, -2147350340, 43, 52,
    84, kHashJackie, 89, 90, 55374, 15, 51534, 91, 363, 5,
    84, kHashClown, 89, 90, 55374, 15, 51534, 91, 363, 5,
    84, 0x00054035, 89, 90, 55374, 15, 51534, 91, 363, 5,
    106, -2147350728, 3, -2147350228, 17, kHashJackie, 43, 51, 14, kHashClown,
    1, 14, 0x00054035, 1, 84, kHashJackie, 88, 315, 5, 3,
    -2147350192, 22, 0, 363, 22, 13, 363, 22, 23, 363,
    73, 77, -2147353612, 15, 0, 351, 43, 50, 352, 4
};

static s32 intro_every_clown[12] = {
    15, 0, 351, 43, 50, 352, 73, 77, -2147353612, 4, 1852989762, 31077
};

static s32 intro_grontar[104] = {
    105, 113, 14, kHashJackie, 73, 14, kHashBarney, 73, 15, 0,
    365, 43, 50, 366, 16, 0, 365, 18, 15, 10,
    365, 16, 10, 365, 1, 115, 0, 11, 117, 114,
    3, -2147350268, 43, 52, 78, kHashJackie, 84, kHashJackie, 89, 90,
    32102, -25470, 78168, 91, 365, 5, 78, kHashBarney, 84, kHashBarney,
    89, 90, 32102, -25470, 78168, 91, 365, 5, 6, -1,
    106, -2147350772, 118, 6, 45, 36, 48921494, 33931, -25378, 78657,
    6, 96, 38, 0x0E9A8D85, 17, kHashJackie, 43, 51, 14, kHashBarney,
    1, 84, kHashJackie, 88, 90, 5, 3, -2147350192, 22, 0,
    365, 22, 10, 365, 73, 77, -2147353124, 15, 0, 355,
    43, 50, 356, 4
};

static s32 intro_every_grontar[10] = {
    15, 0, 355, 43, 50, 356, 73, 77, -2147353124, 4
};

static s32 intro_disco[30] = {
    3, -2147350248, 15, 0, 361, 43, 50, 362, 16, 0,
    361, 18, 15, 12, 361, 16, 12, 361, 1, 115,
    0, 13, 117, 98, 99, 0, 5, 4, 1668507972, 111
};

static s32 play_intro_disco[71] = {
    14, kHashJackie, 1, 6, -1, 6, 2, 14, kHashJackie, 73,
    3, -2147350268, 43, 52, 84, kHashJackie, 89, 90, 27299, 13825,
    3591, 91, 361, 5, 84, kHashDisco, 89, 90, 27299, 13825,
    3591, 91, 361, 5, 6, -1, 106, -2147350640, 118, 17,
    kHashJackie, 43, 51, 14, kHashDisco, 1, 84, kHashJackie, 88, 0,
    5, 3, -2147350204, 3, -2147350192, 22, 0, 361, 22, 12,
    361, 73, 77, -2147352548, 15, 0, 353, 43, 50, 354,
    4
};

static s32 intro_every_disco[10] = {
    15, 0, 353, 43, 50, 354, 73, 77, -2147352548, 4
};

static s32 victory_chef[80] = {
    6, -1, 15, 17, 357, 16, 17, 357, 6, 60,
    105, 42, kHashJackie, 16, 0, 357, 18, 115, 0, 59,
    117, 6, 90, 43, 52, 14, kHashJackie, 1, 84, kHashJackie,
    89, 90, 2670, 2048, 28201, 91, 357, 5, 14, kHashChef,
    1, 84, kHashChef, 89, 90, 2670, 2048, 28201, 91, 357,
    5, 3, -2147350248, 106, -2147350608, 118, 37, 244945277, 2670, 2048,
    28201, 6, 159, 33, kHashChef, 6, 160, 36, 48921494, 2130,
    2239, 28663, 17, kHashJackie, 3, -2147350120, 22, 0, 357, 4
};

static s32 victory_disco[108] = {
    6, -1, 15, 12, 353, 16, 12, 353, 6, 60,
    105, 16, 0, 353, 18, 115, 0, 64, 117, 39,
    6, 90, 3, -2147350248, 14, kHashJackie, 1, 84, kHashJackie, 89,
    90, 28060, 19150, 13000, 5, 43, 55, 28060, 22150, 6800,
    28060, 22150, 13000, 37, 0x02E29489, 28000, 18600, 13000, 37, 0x0E2CA2A5,
    28000, 18500, 13000, 37, 0x0DE2295E, 28000, 18600, 23000, 6, 450,
    43, 46, 43, 52, 14, kHashJackie, 1, 84, kHashJackie, 89,
    90, 34184, 13816, 3608, 91, 353, 5, 14, kHashDisco, 1,
    84, kHashDisco, 89, 90, 34184, 13816, 3608, 91, 353, 5,
    3, -2147350248, 106, -2147350372, 118, 37, 48884857, 34184, 13816, 3608,
    17, kHashJackie, 3, -2147350120, 22, 0, 353, 4
};

static s32 victory_grontar[76] = {
    6, -1, 15, 10, 355, 16, 10, 355, 6, 60,
    105, 16, 0, 355, 18, 115, 0, 58, 117, 6,
    90, 43, 52, 14, kHashJackie, 1, 84, kHashJackie, 89, 90,
    37118, -25467, 80136, 91, 355, 5, 14, kHashBarney, 1, 84,
    kHashBarney, 89, 90, 37118, -25467, 80136, 91, 355, 5, 6,
    -1, 106, -2147350568, 3, -2147350228, 6, 228, 36, 48921494, 34829,
    -25472, 80555, 17, kHashJackie, 3, -2147350120, 22, 0, 355, 14,
    kHashBarney, 1, 22, 10, 355, 4
};

static s32 victory_clown[79] = {
    14, 0x00054035, 73, 6, -1, 15, 13, 351, 16, 13,
    351, 6, 60, 105, 16, 0, 351, 18, 115, 0,
    63, 117, 81, 0x00054035, 6, 90, 43, 52, 14, kHashJackie,
    1, 84, kHashJackie, 89, 90, 55374, 15, 51534, 91, 351,
    5, 14, kHashClown, 1, 84, kHashClown, 89, 90, 55374, 15,
    51534, 91, 351, 5, 6, -1, 106, -2147350488, 3, -2147350228,
    6, 130, 32, kHashClown, 30, 17, kHashJackie, 3, -2147350120, 22,
    0, 351, 14, kHashClown, 1, 22, 13, 351, 4
};

// Sound scripts for boss NIS sequences (packed event data)
static s32 sndintrochef[18] = {
    268447745, 268447752, 268447762, 268447770, 268542070, 268509340, 268542131,
    268447916, 268542131, 268447924, 268542133, 268689606, 268447935, 268689604,
    268480714, 268447954, 268447963, 0
};

static s32 sndintrogrontar[11] = {
    268447751, 268447759, 268972062, 268447786, 268787755, 268447818, 268447833,
    268542049, 268447854, 268509299, 0
};

static s32 sndintroclown[22] = {
    268541973, 268476441, 268541981, 268480546, 268447783, 268447788, 268542009,
    268447824, 268435570, 268435575, 268435587, 268435619, 268435636, 268542143,
    268566724, 268542156, 268484817, 268484823, 268447965, 268570863, 268448002, 0
};

static s32 sndintrodisco[8] = {
    268447745, 268447753, 268447764, 268447771, 268447891, 268447999, 268448199, 0
};

static s32 sndvicchef[10] = {
    268447780, 268513322, 268447801, 268542038, 268542112, 268927137, 268447954,
    268484838, 269070566, 0
};

static s32 sndvicgrontar[20] = {
    268447787, 269058109, 268447810, 268447812, 268447832, 268447836, 269058146,
    268447851, 269058193, 268542124, 268484835, 268787943, 268542188, 268484844,
    268447983, 268542254, 268448058, 268448066, 268448074, 0
};

static s32 sndvicclown[29] = {
    268447749, 268447758, 268447762, 268447769, 268447770, 268447793, 268447801,
    268447809, 268447812, 268447826, 268447843, 268447846, 268447855, 268447860,
    268447862, 268447877, 268447894, 268484761, 268447911, 268447927, 268484791,
    268484794, 268447946, 268447977, 268447985, 269054206, 268448010, 268448011, 0
};

static s32 sndvicdisco[8] = {
    268447933, 268447941, 268447951, 268497114, 268497124, 268542186, 268509435, 0
};

// PSX: china NIS sound tables at 0x800D69E4/0x800D6A44/0x800D6A84.
// Packed format: high nibble event type, middle sound id, low 12 bits frame.
static s32 sndchina1[24] = {
    0x1001200A, 0x2000B00B, 0x3000B05E, 0x20020062, 0x3002007B,
    0x1000C096, 0x10088096, 0x1000C0AD, 0x100030AE, 0x100030B0,
    0x100030B9, 0x100030C0, 0x100880C2, 0x100030E2, 0x100030F4,
    0x1001B110, 0x1001B11A, 0x10003129, 0x10083130, 0x1001A14A,
    0x1001A155, 0x1001A162, 0x1000316D, 0x00000000,
};

static s32 sndchina2[16] = {
    0x10003001, 0x10003009, 0x1000700C, 0x1000300F, 0x10003020,
    0x10003028, 0x1000302D, 0x1001A033, 0x1008303A, 0x10003050,
    0x1001A05B, 0x1000B06F, 0x1000B075, 0x10003083, 0x10003086,
    0x00000000,
};

static s32 sndchina3[18] = {
    0x10003003, 0x1000300B, 0x10003013, 0x1000301B, 0x10003021,
    0x10082034, 0x1001A04B, 0x1001A063, 0x1000B070, 0x1000307B,
    0x10003080, 0x1000308D, 0x1001A091, 0x1001A09D, 0x100030B8,
    0x100030C2, 0x100030C9, 0x00000000,
};

// PSX: sewer NIS sound tables at 0x800D6B44/0x800D6BA0/0x800D6C00.
// Packed format: high nibble event type, middle sound id, low 12 bits frame.
static s32 sndsewer1[23] = {
    0x10099000, 0x10003002, 0x1009D007, 0x10003018, 0x10012036,
    0x10088040, 0x10009074, 0x1000306E, 0x10003074, 0x100B7080,
    0x1009D080, 0x10023080, 0x1001A08C, 0x1001A09A, 0x1000D0A6,
    0x100880A7, 0x100010AE, 0x100010B1, 0x100010B7, 0x100010CD,
    0x100890D1, 0x100090D5, 0x00000000,
};

static s32 sndsewer2[24] = {
    0x1000300B, 0x10003016, 0x1000301E, 0x10003026, 0x10003029,
    0x1001A02D, 0x1001A03D, 0x100C7041, 0x1001A046, 0x100C7049,
    0x100C7056, 0x1000305D, 0x1001A061, 0x2000D09B, 0x300000AC,
    0x1001A0BE, 0x1001E0C4, 0x1000D0CE, 0x100010D7, 0x100010DA,
    0x100190D9, 0x100010DD, 0x100010F3, 0x00000000,
};

static s32 sndsewer3[13] = {
    0x1002A000, 0x1009D000, 0x10099014, 0x1009902C, 0x10099041,
    0x10099055, 0x10099067, 0x10099077, 0x1009908A, 0x10099099,
    0x100990A7, 0x100090B3, 0x00000000,
};

// PSX: additional director sound tables in 0x800D6ACC..0x800D6F00.
static s32 sndwater1[9] = {
    0x1001A007, 0x1000C00D, 0x1000301B, 0x1000301F, 0x1001A04D,
    0x1001A070, 0x10003077, 0x1000307A, 0x00000000,
};

static s32 sndwater2[14] = {
    0x10003007, 0x1001A00B, 0x10012013, 0x2000B014, 0x100C6037,
    0x3000005C, 0x1000A06A, 0x10011071, 0x100000CB, 0x100000D6,
    0x1001A0E1, 0x100000F4, 0x100000F7, 0x00000000,
};

static s32 sndwater3[7] = {
    0x10093000, 0x1000C02C, 0x1000303B, 0x10003041, 0x10003046,
    0x10003074, 0x00000000,
};

static s32 sndroof1[40] = {
    0x10003002, 0x1000300A, 0x10003013, 0x1000301B, 0x10003022,
    0x1008302C, 0x10003044, 0x1008304E, 0x10003062, 0x1001A067,
    0x1001A071, 0x1008406E, 0x1008608F, 0x10086098, 0x100030AE,
    0x100030BD, 0x100110D0, 0x100030DE, 0x100030E4, 0x100030EC,
    0x100030F4, 0x100030FC, 0x10095102, 0x1000C139, 0x1000C141,
    0x10003142, 0x10003145, 0x10003148, 0x10003162, 0x1000316F,
    0x10003177, 0x10003186, 0x1001A19F, 0x1001A1A9, 0x100031B8,
    0x1001A1C7, 0x1001A1D0, 0x100031E6, 0x100031E7, 0x00000000,
};

static s32 sndroof2[34] = {
    0x1000100F, 0x1000301E, 0x1001A02C, 0x1001A036, 0x10083039,
    0x10003045, 0x1000304D, 0x1000304F, 0x1001A04F, 0x10025058,
    0x1008A059, 0x10003060, 0x1001206A, 0x2000B06B, 0x10012087,
    0x100120B7, 0x100260B8, 0x100220B8, 0x1001A0B8, 0x300000C6,
    0x1000C0F5, 0x1000C0FD, 0x100030FE, 0x10003100, 0x10003101,
    0x10094118, 0x1001A120, 0x1000312C, 0x10003132, 0x10003139,
    0x10003140, 0x10003147, 0x10003166, 0x00000000,
};

static s32 sndroof3[18] = {
    0x10093000, 0x10087000, 0x1001A03B, 0x1000806A, 0x10029090,
    0x100290A7, 0x100030AB, 0x100030AE, 0x100030B8, 0x100030BE,
    0x100030E7, 0x100030E8, 0x1001A128, 0x1001A133, 0x1001A143,
    0x10003149, 0x1000314A, 0x00000000,
};

static s32 sndfactory1[30] = {
    0x10099002, 0x10003006, 0x10083012, 0x1000A01C, 0x1000A026,
    0x1000302E, 0x10003036, 0x10003039, 0x1001A045, 0x100B7045,
    0x1000304A, 0x1001A04F, 0x10096053, 0x10090053, 0x1000305A,
    0x10003061, 0x1001A064, 0x1000306A, 0x1000306F, 0x1001A070,
    0x10012074, 0x10003075, 0x10096078, 0x1008A079, 0x1000307A,
    0x10003080, 0x1001A091, 0x10096093, 0x1000C0A7, 0x00000000,
};

static s32 sndfactory2[22] = {
    0x20022000, 0x10003003, 0x1000300B, 0x1009A00F, 0x10003011,
    0x1000301F, 0x10003026, 0x1000304C, 0x1000305B, 0x1009A0B6,
    0x100030BA, 0x100030BE, 0x1001A0DA, 0x1000C0EA, 0x100030F2,
    0x100030F6, 0x1001A102, 0x1001A10C, 0x10022122, 0x1000312D,
    0x30022135, 0x00000000,
};

static s32 sndfactory3[22] = {
    0x10003001, 0x10003009, 0x10003012, 0x10003017, 0x1001A020,
    0x10056027, 0x1001A02A, 0x1001A02C, 0x1006503E, 0x1008903F,
    0x1002F048, 0x1002F04F, 0x1009308E, 0x100930B3, 0x1001A0C8,
    0x1000C0E2, 0x100030EF, 0x100030F1, 0x100030FA, 0x10003100,
    0x10003120, 0x00000000,
};

static s32 sndtable_800D6ECC[3] = {
    0x1000300D, 0x100030A6, 0x00000000,
};

static s32 sndtable_800D6ED8[7] = {
    0x1000301A, 0x1000302E, 0x10003058, 0x1000306C, 0x10003091,
    0x1000309E, 0x00000000,
};

static s32 sndtable_800D6EF4[3] = {
    0x10007012, 0x10007075, 0x00000000,
};

static s32 sndtable_800D6F00[5] = {
    0x10007011, 0x1000702D, 0x1000302F, 0x10003081, 0x00000000,
};

// PSX: fade script at 0x800CC36C.
// Called by victory_poor/ok/good/perfect before tally/dialog branches.
static s32 fade[8] = {
    98, 101, 10, 100, 0, 5, 105, 4
};

// Boss NIS helper scripts (subroutines called by intro/victory scripts)
static s32 boss_setup[18] = {
    9, 20, 73, 74, 42, kHashJackie, 14, kHashJackie, 1, 14,
    kHashJackie, 73, 78, kHashJackie, 41, 6, -1, 4
};

static s32 boss_genericStartNIS[5] = {
    3, -2147350340, 3, -2147350060, 4
};

static s32 boss_victory_widescreen[5] = {
    3, -2147350060, 6, -1, 4
};

static s32 boss_dialog_and_delayed_widescreen[6] = {
    118, 6, 1, 3, -2147350060, 4
};

static s32 boss_checkpoint[3] = {
    28, 0x0ABCB8F4, 4
};

static s32 boss_intro_end[18] = {
    84, kHashJackie, 87, 93, 5, 14, kHashJackie, 1, 43, 44,
    98, 99, 0, 5, 73, 75, 8, 4
};

static s32 boss_victory_done[15] = {
    14, kHashJackie, 1, 105, 98, 100, 255, 99, 255, 5,
    6, -1, 6, 1, 4
};

static s32 boss_widescreen_only[13] = {
    98, 102, 40, 103, 256, 100, 255, 99, 255, 104,
    2, 5, 4
};

struct DirectorScriptRegion {
    u32 virtualBase = 0;
    s32* hostBase = nullptr;
    s32 wordCount = 0;
};

static constexpr s32 kMaxDirectorScriptRegions = 128;
static DirectorScriptRegion g_directorScriptRegions[kMaxDirectorScriptRegions] = {};
static s32 g_directorScriptRegionCount = 0;

template <size_t N>
constexpr s32 ScriptArrayWordCount(const s32(&)[N]) {
    return static_cast<s32>(N);
}

static u32 ToVirtualAddress(const s32* ptr) {
    return static_cast<u32>(reinterpret_cast<uintptr_t>(ptr));
}

static void RegisterScriptRegion(u32 virtualBase, s32* hostBase, s32 wordCount) {
    if (!virtualBase || !hostBase || wordCount <= 0) {
        return;
    }

    for (s32 i = 0; i < g_directorScriptRegionCount; i++) {
        DirectorScriptRegion& region = g_directorScriptRegions[i];
        if (region.virtualBase == virtualBase) {
            region.hostBase = hostBase;
            region.wordCount = wordCount;
            return;
        }
    }

    if (g_directorScriptRegionCount >= kMaxDirectorScriptRegions) {
        return;
    }

    DirectorScriptRegion& next = g_directorScriptRegions[g_directorScriptRegionCount++];
    next.virtualBase = virtualBase;
    next.hostBase = hostBase;
    next.wordCount = wordCount;
}

static s32* ResolveScriptAddress(u32 virtualAddress) {
    for (s32 i = 0; i < g_directorScriptRegionCount; i++) {
        const DirectorScriptRegion& region = g_directorScriptRegions[i];
        if (virtualAddress < region.virtualBase) {
            continue;
        }

        const u32 byteOffset = virtualAddress - region.virtualBase;
        if ((byteOffset & 3) != 0) {
            continue;
        }

        const s32 wordOffset = static_cast<s32>(byteOffset >> 2);
        if (wordOffset < 0 || wordOffset >= region.wordCount) {
            continue;
        }

        return region.hostBase + wordOffset;
    }

    return nullptr;
}

static s32* ResolveOverlay7BlobAddress(u32 virtualAddress) {
    if (virtualAddress < kDirectorOverlay7ScriptBase || virtualAddress >= kDirectorOverlay7ScriptEndExclusive) {
        return nullptr;
    }

    const u32 byteOffset = virtualAddress - kDirectorOverlay7ScriptBase;
    if ((byteOffset & 3) != 0) {
        return nullptr;
    }

    const s32 wordOffset = static_cast<s32>(byteOffset >> 2);
    const s32 blobWordCount = ScriptArrayWordCount(g_directorScriptsOverlay7_8001FA3C);
    if (wordOffset < 0 || wordOffset >= blobWordCount) {
        return nullptr;
    }

    return g_directorScriptsOverlay7_8001FA3C + wordOffset;
}

static void RegisterKnownDirectorScriptRegions() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    auto registerCompiledOverlayScript = [](u32 virtualBase, s32* hostBase, s32 wordCount) {
        RegisterScriptRegion(virtualBase, hostBase, wordCount);
        RegisterScriptRegion(ToVirtualAddress(hostBase), hostBase, wordCount);
    };

    // Register only the overlay scripts and sound tables that we have explicitly
    // reconstructed into host arrays, using their original PSX virtual addresses.
    registerCompiledOverlayScript(0x8001FA3Cu, intro_all_dante, ScriptArrayWordCount(intro_all_dante));
    registerCompiledOverlayScript(0x8001FA54u, intro_chef, ScriptArrayWordCount(intro_chef));
    registerCompiledOverlayScript(0x8001FBCCu, intro_every_chef, ScriptArrayWordCount(intro_every_chef));
    registerCompiledOverlayScript(0x8001FBFCu, intro_clown, ScriptArrayWordCount(intro_clown));
    registerCompiledOverlayScript(0x8001FDB4u, intro_every_clown, ScriptArrayWordCount(intro_every_clown));
    registerCompiledOverlayScript(0x8001FDE4u, intro_grontar, ScriptArrayWordCount(intro_grontar));
    registerCompiledOverlayScript(0x8001FF84u, intro_every_grontar, ScriptArrayWordCount(intro_every_grontar));
    registerCompiledOverlayScript(0x8001FFACu, intro_disco, ScriptArrayWordCount(intro_disco));
    registerCompiledOverlayScript(0x80020024u, play_intro_disco, ScriptArrayWordCount(play_intro_disco));
    registerCompiledOverlayScript(0x80020140u, intro_every_disco, ScriptArrayWordCount(intro_every_disco));
    registerCompiledOverlayScript(0x80020168u, victory_chef, ScriptArrayWordCount(victory_chef));
    registerCompiledOverlayScript(0x800202A8u, victory_disco, ScriptArrayWordCount(victory_disco));
    registerCompiledOverlayScript(0x80020458u, victory_grontar, ScriptArrayWordCount(victory_grontar));
    registerCompiledOverlayScript(0x80020588u, victory_clown, ScriptArrayWordCount(victory_clown));
    registerCompiledOverlayScript(0x800206C4u, sndintrochef, ScriptArrayWordCount(sndintrochef));
    registerCompiledOverlayScript(0x8002070Cu, sndintrogrontar, ScriptArrayWordCount(sndintrogrontar));
    registerCompiledOverlayScript(0x80020738u, sndintroclown, ScriptArrayWordCount(sndintroclown));
    registerCompiledOverlayScript(0x80020790u, sndintrodisco, ScriptArrayWordCount(sndintrodisco));
    registerCompiledOverlayScript(0x800207B0u, sndvicchef, ScriptArrayWordCount(sndvicchef));
    registerCompiledOverlayScript(0x800207D8u, sndvicgrontar, ScriptArrayWordCount(sndvicgrontar));
    registerCompiledOverlayScript(0x80020828u, sndvicclown, ScriptArrayWordCount(sndvicclown));
    registerCompiledOverlayScript(0x8002089Cu, sndvicdisco, ScriptArrayWordCount(sndvicdisco));
    registerCompiledOverlayScript(0x800D69E4u, sndchina1, ScriptArrayWordCount(sndchina1));
    registerCompiledOverlayScript(0x800D6A44u, sndchina2, ScriptArrayWordCount(sndchina2));
    registerCompiledOverlayScript(0x800D6A84u, sndchina3, ScriptArrayWordCount(sndchina3));
    registerCompiledOverlayScript(0x800D6ACCu, sndwater1, ScriptArrayWordCount(sndwater1));
    registerCompiledOverlayScript(0x800D6AF0u, sndwater2, ScriptArrayWordCount(sndwater2));
    registerCompiledOverlayScript(0x800D6B28u, sndwater3, ScriptArrayWordCount(sndwater3));
    registerCompiledOverlayScript(0x800D6B44u, sndsewer1, ScriptArrayWordCount(sndsewer1));
    registerCompiledOverlayScript(0x800D6BA0u, sndsewer2, ScriptArrayWordCount(sndsewer2));
    registerCompiledOverlayScript(0x800D6C00u, sndsewer3, ScriptArrayWordCount(sndsewer3));
    registerCompiledOverlayScript(0x800D6C34u, sndroof1, ScriptArrayWordCount(sndroof1));
    registerCompiledOverlayScript(0x800D6CD4u, sndroof2, ScriptArrayWordCount(sndroof2));
    registerCompiledOverlayScript(0x800D6D5Cu, sndroof3, ScriptArrayWordCount(sndroof3));
    registerCompiledOverlayScript(0x800D6DA4u, sndfactory1, ScriptArrayWordCount(sndfactory1));
    registerCompiledOverlayScript(0x800D6E1Cu, sndfactory2, ScriptArrayWordCount(sndfactory2));
    registerCompiledOverlayScript(0x800D6E74u, sndfactory3, ScriptArrayWordCount(sndfactory3));
    registerCompiledOverlayScript(0x800D6ECCu, sndtable_800D6ECC, ScriptArrayWordCount(sndtable_800D6ECC));
    registerCompiledOverlayScript(0x800D6ED8u, sndtable_800D6ED8, ScriptArrayWordCount(sndtable_800D6ED8));
    registerCompiledOverlayScript(0x800D6EF4u, sndtable_800D6EF4, ScriptArrayWordCount(sndtable_800D6EF4));
    registerCompiledOverlayScript(0x800D6F00u, sndtable_800D6F00, ScriptArrayWordCount(sndtable_800D6F00));
    registerCompiledOverlayScript(0x800208BCu, boss_setup, ScriptArrayWordCount(boss_setup));
    registerCompiledOverlayScript(0x80020904u, boss_genericStartNIS, ScriptArrayWordCount(boss_genericStartNIS));
    registerCompiledOverlayScript(0x80020918u, boss_victory_widescreen, ScriptArrayWordCount(boss_victory_widescreen));
    registerCompiledOverlayScript(0x8002092Cu, boss_dialog_and_delayed_widescreen, ScriptArrayWordCount(boss_dialog_and_delayed_widescreen));
    registerCompiledOverlayScript(0x80020944u, boss_checkpoint, ScriptArrayWordCount(boss_checkpoint));
    registerCompiledOverlayScript(0x80020950u, boss_intro_end, ScriptArrayWordCount(boss_intro_end));
    registerCompiledOverlayScript(0x80020998u, boss_victory_done, ScriptArrayWordCount(boss_victory_done));
    registerCompiledOverlayScript(0x800209D4u, boss_widescreen_only, ScriptArrayWordCount(boss_widescreen_only));

    // PSX dump-backed contiguous overlay7 script blob (BOL_REL.BIN).
    // Register after per-script regions so 0x8001FA3C resolves to the full
    // blob instead of only intro_all_dante's short subrange.
    RegisterScriptRegion(0x8001FA3Cu,
                         g_directorScriptsOverlay7_8001FA3C,
                         ScriptArrayWordCount(g_directorScriptsOverlay7_8001FA3C));
    RegisterScriptRegion(ToVirtualAddress(g_directorScriptsOverlay7_8001FA3C),
                         g_directorScriptsOverlay7_8001FA3C,
                         ScriptArrayWordCount(g_directorScriptsOverlay7_8001FA3C));

    RegisterScriptRegion(0x800CC36Cu, fade, ScriptArrayWordCount(fade));
    RegisterScriptRegion(ToVirtualAddress(fade), fade, ScriptArrayWordCount(fade));

    RegisterScriptRegion(0x800D8208u, victory_poor, ScriptArrayWordCount(victory_poor));
    RegisterScriptRegion(0x800D8278u, victory_ok, ScriptArrayWordCount(victory_ok));
    RegisterScriptRegion(0x800D82E8u, victory_good, ScriptArrayWordCount(victory_good));
    RegisterScriptRegion(0x800D8358u, victory_perfect, ScriptArrayWordCount(victory_perfect));

    RegisterScriptRegion(0x800D6F14u, g_directorScripts_800D6F14, ScriptArrayWordCount(g_directorScripts_800D6F14));
    RegisterScriptRegion(ToVirtualAddress(g_directorScripts_800D6F14), g_directorScripts_800D6F14, ScriptArrayWordCount(g_directorScripts_800D6F14));

    RegisterScriptRegion(0x800D7048u, start_generic, ScriptArrayWordCount(start_generic));
    RegisterScriptRegion(0x800D70B0u, start_frantic, ScriptArrayWordCount(start_frantic));
    RegisterScriptRegion(0x800D803Cu, gotopoint, ScriptArrayWordCount(gotopoint));
    RegisterScriptRegion(0x800D8058u, NISladder1, ScriptArrayWordCount(NISladder1));
    RegisterScriptRegion(0x800D83C8u, death, ScriptArrayWordCount(death));
    RegisterScriptRegion(0x800D8454u, death_vol, ScriptArrayWordCount(death_vol));
    RegisterScriptRegion(0x800D84E0u, death_fall_pavement, ScriptArrayWordCount(death_fall_pavement));
    RegisterScriptRegion(0x800D855Cu, death_fall_water, ScriptArrayWordCount(death_fall_water));
    RegisterScriptRegion(0x800D8598u, death_generic, ScriptArrayWordCount(death_generic));
    RegisterScriptRegion(0x800D85C0u, death_fall_goo, ScriptArrayWordCount(death_fall_goo));
    RegisterScriptRegion(0x800D85E4u, NISdoor1, ScriptArrayWordCount(NISdoor1));
    RegisterScriptRegion(0x800D8640u, NISdoor1WithDialog, ScriptArrayWordCount(NISdoor1WithDialog));
    RegisterScriptRegion(0x800D86B0u, levelEnd, ScriptArrayWordCount(levelEnd));
    RegisterScriptRegion(0x800D86CCu, wait_subroutine, ScriptArrayWordCount(wait_subroutine));
    RegisterScriptRegion(0x800D86E0u, defaultBeginScript, ScriptArrayWordCount(defaultBeginScript));
    RegisterScriptRegion(0x800DCD5Cu, start_fall, ScriptArrayWordCount(start_fall));
    RegisterScriptRegion(0x800DCD60u, start_door, ScriptArrayWordCount(start_door));
    RegisterScriptRegion(0x800DCD64u, slot, ScriptArrayWordCount(slot));

    // Host aliases for any scripts compiled into this binary.
    RegisterScriptRegion(ToVirtualAddress(victory_poor), victory_poor, ScriptArrayWordCount(victory_poor));
    RegisterScriptRegion(ToVirtualAddress(victory_ok), victory_ok, ScriptArrayWordCount(victory_ok));
    RegisterScriptRegion(ToVirtualAddress(victory_good), victory_good, ScriptArrayWordCount(victory_good));
    RegisterScriptRegion(ToVirtualAddress(victory_perfect), victory_perfect, ScriptArrayWordCount(victory_perfect));
    RegisterScriptRegion(ToVirtualAddress(start_generic), start_generic, ScriptArrayWordCount(start_generic));
    RegisterScriptRegion(ToVirtualAddress(start_frantic), start_frantic, ScriptArrayWordCount(start_frantic));
    RegisterScriptRegion(ToVirtualAddress(death_fall_pavement), death_fall_pavement, ScriptArrayWordCount(death_fall_pavement));
    RegisterScriptRegion(ToVirtualAddress(death_fall_water), death_fall_water, ScriptArrayWordCount(death_fall_water));
    RegisterScriptRegion(ToVirtualAddress(death_generic), death_generic, ScriptArrayWordCount(death_generic));
    RegisterScriptRegion(ToVirtualAddress(death_fall_goo), death_fall_goo, ScriptArrayWordCount(death_fall_goo));
    RegisterScriptRegion(ToVirtualAddress(NISdoor1), NISdoor1, ScriptArrayWordCount(NISdoor1));
    RegisterScriptRegion(ToVirtualAddress(NISdoor1WithDialog), NISdoor1WithDialog, ScriptArrayWordCount(NISdoor1WithDialog));
    RegisterScriptRegion(ToVirtualAddress(gotopoint), gotopoint, ScriptArrayWordCount(gotopoint));
    RegisterScriptRegion(ToVirtualAddress(NISladder1), NISladder1, ScriptArrayWordCount(NISladder1));
    RegisterScriptRegion(ToVirtualAddress(death), death, ScriptArrayWordCount(death));
    RegisterScriptRegion(ToVirtualAddress(death_vol), death_vol, ScriptArrayWordCount(death_vol));
    RegisterScriptRegion(ToVirtualAddress(levelEnd), levelEnd, ScriptArrayWordCount(levelEnd));
    RegisterScriptRegion(ToVirtualAddress(wait_subroutine), wait_subroutine, ScriptArrayWordCount(wait_subroutine));
    RegisterScriptRegion(ToVirtualAddress(defaultBeginScript), defaultBeginScript, ScriptArrayWordCount(defaultBeginScript));
    RegisterScriptRegion(ToVirtualAddress(start_generic_checkpoint), start_generic_checkpoint, ScriptArrayWordCount(start_generic_checkpoint));
    RegisterScriptRegion(ToVirtualAddress(start_fall), start_fall, ScriptArrayWordCount(start_fall));
    RegisterScriptRegion(ToVirtualAddress(start_door), start_door, ScriptArrayWordCount(start_door));
    RegisterScriptRegion(ToVirtualAddress(slot), slot, ScriptArrayWordCount(slot));

}

static s32 EstimateRuntimeScriptWords(const s32* script, s32 maxWords) {
    if (!script || maxWords <= 0) {
        return 0;
    }

    for (s32 i = 0; i < maxWords; i++) {
        const DirectorOpcode op = static_cast<DirectorOpcode>(script[i]);
        if (op == DirectorOpcode::End || op == DirectorOpcode::EndScript) {
            return i + 1;
        }
    }

    return maxWords;
}

static void RegisterRuntimeScriptRegion(s32* script) {
    if (!script) {
        return;
    }

    const s32 words = EstimateRuntimeScriptWords(script, 1024);
    if (words <= 0) {
        return;
    }

    RegisterScriptRegion(ToVirtualAddress(script), script, words);
}

static s32* ScriptPtrFromWord(s32 word) {
    RegisterKnownDirectorScriptRegions();

    const u32 raw = static_cast<u32>(word);
    if (raw == 0) {
        return nullptr;
    }

    if (s32* overlay7 = ResolveOverlay7BlobAddress(raw)) {
        return overlay7;
    }

    s32* resolved = ResolveScriptAddress(raw);
    if (resolved) {
        return resolved;
    }

    return nullptr;
}

static s32* ResolveDirectorScriptVA(u32 virtualAddress) {
    return ScriptPtrFromWord(static_cast<s32>(virtualAddress));
}

struct DirectorScriptPtrInfo {
    u32 scriptVirtualAddress = 0;
    u32 regionVirtualBase = 0;
    s32 wordOffset = 0;
    s32 regionWordCount = 0;
};

static bool IsPsxScriptVirtualAddress(u32 address) {
    return (address & 0xFF000000u) == 0x80000000u;
}

static bool ResolveDirectorScriptPtrInfo(const s32* ptr, DirectorScriptPtrInfo* outInfo) {
    if (!ptr || !outInfo) {
        return false;
    }

    RegisterKnownDirectorScriptRegions();

    const DirectorScriptRegion* fallbackRegion = nullptr;
    s32 fallbackWordOffset = 0;
    const uintptr_t ptrAddress = reinterpret_cast<uintptr_t>(ptr);

    for (s32 i = 0; i < g_directorScriptRegionCount; i++) {
        const DirectorScriptRegion& region = g_directorScriptRegions[i];
        if (!region.hostBase || region.wordCount <= 0) {
            continue;
        }

        const uintptr_t regionStart = reinterpret_cast<uintptr_t>(region.hostBase);
        const uintptr_t regionEnd = regionStart +
            static_cast<uintptr_t>(region.wordCount) * static_cast<uintptr_t>(sizeof(s32));

        if (ptrAddress < regionStart || ptrAddress >= regionEnd) {
            continue;
        }

        const s32 wordOffset = static_cast<s32>((ptrAddress - regionStart) / sizeof(s32));
        if (!fallbackRegion) {
            fallbackRegion = &region;
            fallbackWordOffset = wordOffset;
        }

        if (IsPsxScriptVirtualAddress(region.virtualBase)) {
            outInfo->regionVirtualBase = region.virtualBase;
            outInfo->wordOffset = wordOffset;
            outInfo->regionWordCount = region.wordCount;
            outInfo->scriptVirtualAddress = region.virtualBase + (static_cast<u32>(wordOffset) << 2);
            return true;
        }
    }

    if (!fallbackRegion) {
        return false;
    }

    outInfo->regionVirtualBase = fallbackRegion->virtualBase;
    outInfo->wordOffset = fallbackWordOffset;
    outInfo->regionWordCount = fallbackRegion->wordCount;
    outInfo->scriptVirtualAddress = fallbackRegion->virtualBase + (static_cast<u32>(fallbackWordOffset) << 2);
    return true;
}

static const char* DirectorScriptNameFromRegionBase(u32 regionBase) {
    switch (regionBase) {
        case 0x8001FDE4u: return "intro_grontar";
        case 0x800208BCu: return "boss_setup";
        case 0x80020950u: return "boss_intro_end";
        case 0x800D83C8u: return "death";
        case 0x800D8454u: return "death_vol";
        case 0x800D84E0u: return "death_fall_pavement";
        case 0x800D855Cu: return "death_fall_water";
        case 0x800D8598u: return "death_generic";
        case 0x800D85C0u: return "death_fall_goo";
        case 0x800D86E0u: return "default_begin";
        default: return "unknown";
    }

    return "unknown";
}

static bool ResolveHudBossNameAddress(u32 address, const char** outName) {
    if (!outName) {
        return false;
    }

    switch (address) {
        case 0x8001FA34u: *outName = "Dante"; return true;
        case 0x8001FA4Cu: *outName = "Chef"; return true;
        case 0x8001FBF4u: *outName = "Clown"; return true;
        case 0x8001FDDCu: *outName = "Barney"; return true;
        case 0x8002001Cu: *outName = "Disco"; return true;
        default:
            return false;
    }
}

static const char* ResolveHudBossNamePtr(u32 address) {
    const char* mappedName = nullptr;
    if (ResolveHudBossNameAddress(address, &mappedName)) {
        return mappedName;
    }

    return nullptr;
}

static const char* ResolveVictoryScriptLabel(const s32* script) {
    if (script == victory_poor) return "victory_poor";
    if (script == victory_ok) return "victory_ok";
    if (script == victory_good) return "victory_good";
    if (script == victory_perfect) return "victory_perfect";
    if (script == victory_disco) return "victory_disco";
    if (script == victory_clown) return "victory_clown";
    if (script == victory_chef) return "victory_chef";
    if (script == victory_grontar) return "victory_grontar";
    if (script == wait_subroutine) return "wait_subroutine";
    return "unknown";
}

static const char* ResolveIntroScriptLabel(const s32* script) {
    if (script == start_generic) return "start_generic";
    if (script == start_frantic) return "start_frantic";
    if (script == intro_all_dante) return "intro_all_dante";
    if (script == intro_every_chef) return "intro_every_chef";
    if (script == intro_every_grontar) return "intro_every_grontar";
    if (script == intro_every_clown) return "intro_every_clown";
    if (script == intro_every_disco) return "intro_every_disco";
    if (script == intro_chef) return "intro_chef";
    if (script == intro_grontar) return "intro_grontar";
    if (script == intro_clown) return "intro_clown";
    if (script == intro_disco) return "intro_disco";
    return "unknown";
}

static s32 GetDirectorFrameCounter() {
    if (!g_time) {
        return 0;
    }
    return static_cast<s32>(g_time->GetFrameCounter());
}

static void PushScriptReturnAddress(Director* director, s32* nextScript) {
    returnAddress[returnAddressIndex] = director->scriptPtr;
    returnAddressIndex++;
    director->scriptPtr = nextScript;
}

static Thing* FindThingInListByScriptRef(ccList& list, u32 ref) {
    ccNode* node = list.FindNodeCRC(ref, nullptr);
    if (!node) {
        return nullptr;
    }

    return static_cast<Thing*>(node);
}

static Humanoid* FindHumanoidByScriptRef(u32 ref) {
    if (!g_ai) {
        return nullptr;
    }

    Thing* thing = FindThingInListByScriptRef(g_ai->humanoidList, ref);
    if (!thing) {
        return nullptr;
    }

    return static_cast<Humanoid*>(thing);
}

static Humanoid* FindHumanoidInHumanoidListByScriptRef(u32 ref) {
    if (!g_ai) {
        return nullptr;
    }

    Thing* thing = FindThingInListByScriptRef(g_ai->humanoidList, ref);
    if (!thing) {
        return nullptr;
    }

    return static_cast<Humanoid*>(thing);
}

static Camera* GetGameCamera() {
    if (!g_display) {
        return nullptr;
    }

    return g_display->GetCamera();
}

static void DisablePlayerInputProcessing() {
    if (Player::s_player && Player::s_player->behaviour) {
        Player::s_player->behaviour->DisableInputProcessing();
    }
}

static s32 GetCurrentWorldLevelID() {
    if (!g_game || !g_game->GetWorld()) {
        return 0;
    }

    return g_game->GetWorld()->GetCurLevelID();
}

static s32 checkPoint(const LVector& pos, s32 blockNum);

// PSX: setJackieCheckpoint__FUl (DIRECTOR.CPP:4697, 0x8003F138)
static s32 setJackieCheckpoint(u32 pointCRC) {
    MARKFUNCTION(0x8003F138);

    WorldPointNode* point = WorldPoints_GetNISPoint(pointCRC);
    if (!point) {
        return 0;
    }

    checkPoint(point->pos, point->parValue);
    return 1;
}

// PSX: checkPoint__FR10tagLVectorl (DIRECTOR.CPP:4705, 0x8003F184)
static s32 checkPoint(const LVector& pos, s32 blockNum) {
    MARKFUNCTION(0x8003F184);

    if (!Player::s_player) {
        return 0;
    }

    Player::s_player->OnCheckpoint();
    Player::s_player->checkpoint.field0 = pos.x;
    Player::s_player->checkpoint.field4 = pos.y;
    Player::s_player->checkpoint.field8 = pos.z;
    Player::s_player->checkpoint.field24 = blockNum;
    return blockNum;
}

// PSX: sets Thing::flags bit 0x200 on all move-list things during script processing.
static void SetDirectorFlagsOnMoveList() {
    if (!g_ai) return;
    for (ccMinNode* n = g_ai->moveList.head; n; n = n->next) {
        Thing* thing = static_cast<Thing*>(n);
        thing->flags |= TF_DIRECTOR_ACTIVE;
    }
}

// PSX: clears Thing::flags bit 0x200 on all move-list things when script ends.
static void ClearDirectorFlagsOnMoveList() {
    if (!g_ai) return;
    for (ccMinNode* n = g_ai->moveList.head; n; n = n->next) {
        Thing* thing = static_cast<Thing*>(n);
        thing->flags &= ~TF_DIRECTOR_ACTIVE;
    }
}

Director* g_director = nullptr;

// PSX: __8Director (DIRECTOR.CPP:2658, 0x800CA1B8)
Director::Director() {
    MARKFUNCTION(0x800CA1B8);
    scriptPtr = nullptr;
    scriptBase = nullptr;
    codeSnipPtr = nullptr;
    wsBarCurrent = 0;
    wsBarTarget = 0;
    wsBarStep = 0;
    wsMode = 2;
    wsAlphaStep = 10;
    wsAlphaCurrent = 0;
    wsAlphaTarget = 0;
    field68 = 0;
    enableInput = 0;
}

// PSX: _._8Director (DIRECTOR.CPP:2681, 0x8003BE10)
Director::~Director() {
    MARKFUNCTION(0x8003BE10);
    if (g_director == this) {
        g_director = nullptr;
    }
}

// PSX: InternalOpen__8Director (DIRECTOR.CPP:2704, 0x800CA2AC)
void Director::InternalOpen() {
    MARKFUNCTION(0x800CA2AC);
    if (g_game) {
        runDirectorHandler = g_game->GetHandlerSet1().AddHandler(runDirector, 20);
        overlayHandler = g_game->GetHandlerSet2().AddHandler(DrawDirectorOverlays, -31);
    }

    Manager::InternalOpen();
}

// PSX: InternalClose__8Director (DIRECTOR.CPP:2757, 0x8003C11C)
void Director::InternalClose() {
    MARKFUNCTION(0x8003C11C);
    cleanUpTexAnim();
    handlerSetA.PurgeHandlers();
    handlerSetB.PurgeHandlers();
    Manager::InternalClose();
}

// PSX: InternalReset__8Director (DIRECTOR.CPP:2736, 0x8003C04C)
void Director::InternalReset() {
    MARKFUNCTION(0x8003C04C);
    scriptState = ToScriptStateValue(DirectorScriptState::Idle);
    timerTarget = 0;
    timerStart = 0;
    scriptPtr = nullptr;
    codeSnipPtr = nullptr;
    scriptBase = nullptr;
    returnAddressIndex = 0;
    wsBarTarget = 0;
    wsBarCurrent = 0;
    wsMode = 2;
    wsAlphaStep = 10;
    wsAlphaCurrent = 0;
    wsAlphaTarget = 0;

    handlerSetB.PurgeHandlers();
    enableInput = 1;
}

// PSX: LevelReset__8Director (DIRECTOR.CPP:2731, 0x8003C044)
void Director::LevelReset() {
    MARKFUNCTION(0x8003C044);
    visitedLevels = 0;
}

// PSX: SetScript__8Director (DIRECTOR.CPP:2765, 0x8003C234)
void Director::SetScript() {
    MARKFUNCTION(0x8003C234);

    // PSX: a1[0x1C] = a1[0x24] = defaultBeginScript; a1[0xA8] = 534;
    //      gp[0x424] = -1; gp[0x420] = 0; gp[0x428] = 180.
    scriptPtr = ResolveDirectorScriptVA(kDirectorScriptDefaultBegin);
    codeSnipPtr = scriptPtr;
    scriptState = ToScriptStateValue(DirectorScriptState::SetScript);

    directorDialogCounter = -1;
    returnAddressIndex = 0;
    directorDialogLimit = 180;
}

// PSX: SetCodeSnip__8DirectorPlP5Thing (DIRECTOR.CPP:2782, 0x8003C268)
void Director::SetCodeSnip(s32* snip, Thing* thing) {
    MARKFUNCTION(0x8003C268);

    RegisterKnownDirectorScriptRegions();

    scriptState = ToScriptStateValue(DirectorScriptState::SetCodeSnip);

    // PSX: SetCodeSnip reinitializes Call/Return and dialog wait globals.
    returnAddressIndex = 0;
    directorDialogCounter = -1;
    directorDialogLimit = 180;

    scriptPtr = snip;
    RegisterRuntimeScriptRegion(scriptPtr);
    codeSnipPtr = snip;
    g_codeSnipThing = thing; // PSX: gp[869] = thing

    const s32* deathScript = ResolveDirectorScriptVA(kDirectorScriptDeath);
    const s32* deathVolScript = ResolveDirectorScriptVA(kDirectorScriptDeathVol);
    if (snip == deathScript || snip == deathVolScript || snip == death || snip == death_vol) {
        DirectorScriptPtrInfo info = {};
        if (ResolveDirectorScriptPtrInfo(snip, &info)) {
            LOG("[Director] SetCodeSnip death-entry script=0x%08X base=0x%08X (%s) offset=%d/%d deathType=%d thingType=%u uid=%u",
                info.scriptVirtualAddress,
                info.regionVirtualBase,
                DirectorScriptNameFromRegionBase(info.regionVirtualBase),
                info.wordOffset,
                info.regionWordCount,
                deathType,
                thing ? static_cast<u32>(thing->thingType) : 0u,
                thing ? static_cast<u32>(thing->uniqueID) : 0u);
        }
        else {
            LOG("[Director] SetCodeSnip death-entry script_ptr=%p deathType=%d thingType=%u uid=%u",
                static_cast<const void*>(snip),
                deathType,
                thing ? static_cast<u32>(thing->thingType) : 0u,
                thing ? static_cast<u32>(thing->uniqueID) : 0u);
        }
    }

    returnAddressIndex = 0;
    directorDialogLimit = 180;
}

bool Director::TriggerDeathVolume(s32 newDeathType) {
    RegisterKnownDirectorScriptRegions();

    s32* deathVolScript = ResolveDirectorScriptVA(kDirectorScriptDeathVol);
    if (codeSnipPtr == deathVolScript || codeSnipPtr == death_vol) {
        LOG("[Director] TriggerDeathVolume ignored (already death_vol) deathType=%d", newDeathType);
        return false;
    }

    // Do not allow a death volume to interrupt a door NIS.
    if (g_codeSnipThing) {
        u16 thingType = g_codeSnipThing->thingType;
        if (thingType == AITypes::TT_DOOR) {
            LOG("[Director] TriggerDeathVolume ignored (door NIS in progress) deathType=%d", newDeathType);
            return false;
        }
    }

    LOG("[Director] TriggerDeathVolume deathType=%d", newDeathType);
    deathType = newDeathType;
    SetCodeSnip(deathVolScript, nullptr);
    return true;
}

void Director::TriggerGotoPoint(s32 x, s32 y, s32 z, Thing* thing) {
    RegisterKnownDirectorScriptRegions();

    nisPointX = x;
    nisPointY = y;
    nisPointZ = z;
    SetCodeSnip(ResolveDirectorScriptVA(kDirectorScriptGotoPoint), thing);
}

// PSX: Process__8Director (DIRECTOR.CPP:2806, 0x8003C298)
void Director::Process() {
    MARKFUNCTION(0x8003C298);

    // PC: PSX called HandleWideScreen from the same draw-adjacent function as

    struct WideScreenStepGuard {
        Director* self;
        ~WideScreenStepGuard() { self->HandleWideScreen(); }
    } wideScreenStepGuard{this};

    // PSX iterates the two internal handler chains BEFORE the scriptPtr null check.
    // Each node: lw $v0, 0x18($a0) = funcPtr; lw $s0, 0($a0) = next; jalr $v0 (no args).
    for (ccMinNode* node = handlerSetA.handlerList.head; node; ) {
        Handler* handler = static_cast<Handler*>(node);
        node = node->next;
        if (handler->funcPtr) {
            handler->funcPtr(handler);
        }
    }

    for (ccMinNode* node = handlerSetB.handlerList.head; node; ) {
        Handler* handler = static_cast<Handler*>(node);
        node = node->next;
        if (handler->funcPtr) {
            handler->funcPtr(handler);
        }
    }

    // PSX: if scriptPtr (a1+0x1C) is null, return 1 with no further work.
    if (!scriptPtr) {
        return;
    }

    // PSX: if enableInput==0, disable player input processing
    if (!enableInput) {
        DisablePlayerInputProcessing();
    }

    // PSX: set Thing::flags bit 0x200 on all move-list things
    SetDirectorFlagsOnMoveList();

    // PSX: compute gp+0x42C scratch elapsed value = frameCounter - timerStart
    const s32 elapsed = GetDirectorFrameCounter() - timerStart;
    directorElapsedFrames = (elapsed < 0) ? 0 : elapsed;

    if (scriptBase && *scriptBase) {
        ProcessSoundScript();
    }

    field68 = 0;

    // PSX: while(1) with field68 check at top
    for (s32 i = 0; i < 512; i++) {
        if (field68) {
            return;
        }

        if (!scriptPtr) {
            return;
        }

        s32* const codeSnipBeforeDispatch = codeSnipPtr;
        const DirectorOpcode opcode = static_cast<DirectorOpcode>(*scriptPtr);

        switch (opcode) {
            case DirectorOpcode::End:
                // PSX: clear director state, clear NIS flags, destroy directorSound
                scriptPtr = nullptr;
                codeSnipPtr = nullptr;
                g_codeSnipThing = nullptr;
                scriptState = ToScriptStateValue(DirectorScriptState::Idle);
                ClearDirectorFlagsOnMoveList();
                if (directorSound) {
                    delete directorSound;
                    directorSound = nullptr;
                }
                return;

            case DirectorOpcode::ResetTimeout:
                // PSX: clears gp+20 (`directorTimeOut`) checked by gsPlayState.
                g_directorActive = 0;
                scriptPtr += 1;
                break;

            case DirectorOpcode::EndScript:
                // PSX Process case 2 (0x8003C298): only the visitedLevels bit is gated on
                // scriptState==SetScript (534); the teardown itself is unconditional, so
                // EndScript also terminates SetCodeSnip (537) NIS/volume scripts.
                if (scriptState == ToScriptStateValue(DirectorScriptState::SetScript) && g_game && g_game->GetWorld()) {
                    const s32 levelID = GetCurrentWorldLevelID();
                    if (levelID >= 0 && levelID < 31) {
                        visitedLevels |= (1 << levelID);
                    }
                }
                scriptPtr = nullptr;
                codeSnipPtr = nullptr;
                g_codeSnipThing = nullptr;
                scriptState = ToScriptStateValue(DirectorScriptState::Idle);
                ClearDirectorFlagsOnMoveList();
                if (directorSound) {
                    delete directorSound;
                    directorSound = nullptr;
                }
                return;

            case DirectorOpcode::Call:
                returnAddress[returnAddressIndex] = scriptPtr + 2;
                returnAddressIndex++;
                scriptPtr = ScriptPtrFromWord(scriptPtr[1]);
                break;

            case DirectorOpcode::Return:
            {
                if (returnAddressIndex > 0) {
                    returnAddressIndex--;
                    scriptPtr = returnAddress[returnAddressIndex];
                }
                else {
                    scriptPtr = &returnUnderflowScriptWord;
                }
                break;
            }

            case DirectorOpcode::Yield:
                // PSX opcode 5: consume one word and block script execution for this frame.
                scriptPtr += 1;
                field68 = 1;
                break;

            case DirectorOpcode::Timer:
            {
                // PSX: Timer returns 1=done, 0=blocked
                s32 done = TimerStep();
                if (done) {
                    field68 = 0;
                }
                else {
                    field68 = 1;
                }
                if (field68) {
                    return;
                }
                break;
            }

            case DirectorOpcode::Loop:
                // PSX: Loop() is empty (8 bytes = jr $ra; nop) and scriptPtr is NOT
                // advanced in the outer handler — the opcode loops back to itself every
                // frame, keeping the script blocked indefinitely.
                Loop();
                field68 = 1;
                break;

            case DirectorOpcode::EnablePlayerInput:
            {
                enableInput = 1;
                scriptPtr += 1;
                break;
            }

            case DirectorOpcode::DisablePlayerInput:
            {
                enableInput = 0;
                scriptPtr += 1;
                break;
            }

            case DirectorOpcode::DetermineLevelIntro:
                scriptPtr += 1;
                DetermineLevelIntro();
                break;

            case DirectorOpcode::FaceThing:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;

                Humanoid* humanoid = FindHumanoidInHumanoidListByScriptRef(thingRef);
                if (humanoid) {
                    Camera* camera = GetGameCamera();
                    if (camera) {
                        humanoid->FacePoint(camera->GetPosition(), 0);
                    }
                    else {
                        LOG("[Director] FaceThing missing camera for ref=0x%08X", thingRef);
                    }
                    humanoid->SetDesiredMoveDirection(humanoid->orientation.y);
                }
                else {
                    // PSX: logs "Can't find thing to face" if lookup fails.
                    LOG("[Director] FaceThing unresolved ref=0x%08X", thingRef);
                }
                break;
            }

            case DirectorOpcode::SetHumanoidAction:
            {
                // PSX: reads thingRef, calls vtable+232 (SetActionState)
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;
                const s32 requestedState = *scriptPtr;
                Humanoid* humanoid = FindHumanoidInHumanoidListByScriptRef(thingRef);
                if (humanoid) {
                    humanoid->SetActionState(static_cast<u32>(requestedState), 0);
                }

                scriptPtr += 1;
                break;
            }

            case DirectorOpcode::DynamicAnimLoad:
            case DirectorOpcode::DynamicAnimWaitLoaded:
            case DirectorOpcode::DynamicAnimWaitCamera:
            case DirectorOpcode::DynamicAnimUnload:
                ProcessDynamicAnimFunc();
                break;

            case DirectorOpcode::WaitAnimationDone:
            {
                s32 done = WaitAnimationDoneStep();
                if (done) {
                    field68 = 0;
                }
                else {
                    field68 = 1;
                }
                if (field68) {
                    return;
                }
                break;
            }

            case DirectorOpcode::WaitForNisControl:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                Humanoid* humanoid = FindHumanoidInHumanoidListByScriptRef(thingRef);
                if (humanoid && humanoid->behaviour &&
                    humanoid->behaviour->handlerDispatch == -1 &&
                    humanoid->behaviour->handler == Behaviour::NisControl) {
                    // Still walking to destination - block script
                    return;
                }
                scriptPtr += 2;
                break;
            }

            case DirectorOpcode::RestorePlayerControl:
                // PSX: restores player to PlayerUserControl behaviour
                // Sets behaviour flags back to normal input processing
                if (Player::s_player) {
                    if (Player::s_player->behaviour) {
                        Player::s_player->behaviour->handlerThisOffset = 0;
                        Player::s_player->behaviour->handlerDispatch = -1;
                        Player::s_player->behaviour->handler = Behaviour::PlayerUserControl;
                    }
                }
                scriptPtr += 1;
                break;

            case DirectorOpcode::PlayThingDynamicAnim:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                const s32 animEnumVal = scriptPtr[2];
                scriptPtr += 3;
                Humanoid* humanoid = FindHumanoidInHumanoidListByScriptRef(thingRef);
                if (humanoid && humanoid->model) {
                    SModel* sm = static_cast<SModel*>(humanoid->model);
                    sm->PlayDynamicAnim(animEnumVal);
                }
                break;
            }

            case DirectorOpcode::SetupFaceTextureAnim:
            {
                if (p3d::inventory) {
                    auto* texAnim = p3d::inventory->Find<tRAMTexAnim>("TChanFace~G");
                    texAnimA = reinterpret_cast<uintptr_t>(texAnim);
                    if (texAnim) {
                        auto* flipbook = texAnim->MakePuppet();
                        flipbook->SetCycleCallback(p3dFwdCycle);
                        flipbookA = reinterpret_cast<uintptr_t>(flipbook);
                    }
                    else {
                        flipbookA = 0;
                    }

                    texAnim = p3d::inventory->Find<tRAMTexAnim>("CChanFace~G");
                    texAnimB = reinterpret_cast<uintptr_t>(texAnim);
                    if (texAnim) {
                        auto* flipbook = texAnim->MakePuppet();
                        flipbook->SetCycleCallback(p3dFwdCycle);
                        flipbookB = reinterpret_cast<uintptr_t>(flipbook);
                    }
                    else {
                        flipbookB = 0;
                    }
                }
                scriptPtr += 1;
                break;
            }

            case DirectorOpcode::CleanupFaceTextureAnim:
                scriptPtr += 1;
                cleanUpTexAnim();
                break;

            case DirectorOpcode::QueueDetermineNextState:
            {
                const s32* queueOpcodePtr = scriptPtr;
                // PSX: reads optional level ID param, sets g_selectedLevel if != -1
                const s32 levelParam = scriptPtr[1];
                scriptPtr += 2;

                DirectorScriptPtrInfo info = {};
                if (ResolveDirectorScriptPtrInfo(queueOpcodePtr, &info)) {
                    LOG("[Director] QueueDetermineNextState script=0x%08X base=0x%08X (%s) word=%d/%d levelParam=%d returnDepth=%d",
                        info.scriptVirtualAddress,
                        info.regionVirtualBase,
                        DirectorScriptNameFromRegionBase(info.regionVirtualBase),
                        info.wordOffset,
                        info.regionWordCount,
                        levelParam,
                        returnAddressIndex);
                }
                else {
                    LOG("[Director] QueueDetermineNextState script_ptr=%p levelParam=%d returnDepth=%d",
                        static_cast<const void*>(queueOpcodePtr),
                        levelParam,
                        returnAddressIndex);
                }

                if (levelParam != -1) {
                    extern s16 g_selectedLevel;
                    g_selectedLevel = static_cast<s16>(levelParam);
                }
                if (g_game) {
                    g_game->SetState(GameState::DetermineNextGameState);
                }
                break;
            }

            case DirectorOpcode::SetGameState:
            {
                const s32 gameState = scriptPtr[1];
                scriptPtr += 2;
                if (g_game && gameState >= 0 && gameState < static_cast<s32>(GameState::COUNT)) {
                    g_game->SetState(static_cast<GameState>(gameState));
                }
                field68 = 1;
                return;
            }

            case DirectorOpcode::SetCheckpoint:
                scriptPtr += 1;
                if (Player::s_player) {
                    Player::s_player->OnCheckpoint();
                }
                break;

            case DirectorOpcode::SetCheckpointByUid:
            {
                // PSX: reads UID, calls setJackieCheckpoint(uid).
                // If checkpoint set fails, calls OnCheckpoint as fallback.
                const u32 pointCRC = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;

                if (!setJackieCheckpoint(pointCRC) && Player::s_player) {
                    Player::s_player->OnCheckpoint();
                }

                // PSX DIRECTOR.CPP:3517 clears checkpoint +44/+48 after this opcode.
                if (Player::s_player) {
                    Player::s_player->checkpoint.field44 = 0;
                    Player::s_player->checkpoint.field48 = 0;
                }
                break;
            }

            case DirectorOpcode::SetCheckpointData:
                // PSX: dword_800D6F90 = scriptPtr[1]. On PSX 0x800D6F90 is BOTH a
                // variable and the argument word of the shared cleanup script's
                // SetCheckpointByUid at 0x800D6F8C (self-modifying script!). Levels
                // poke their landing-checkpoint CRC here, call the cleanup, then
                // restore the default. Write through to the blob word so the
                // cleanup consumes the live value like PSX does.
                g_checkpointDataValue = scriptPtr[1];
                if (s32* liveWord = ResolveDirectorScriptVA(0x800D6F90u)) {
                    *liveWord = scriptPtr[1];
                }
                scriptPtr += 2;
                break;

            case DirectorOpcode::TriggerCheckpoint:
                scriptPtr += 1;
                if (Player::s_player) {
                    Player::s_player->OnCheckpoint();
                }
                break;

            case DirectorOpcode::ClearCheckpointValid:
                // PSX: SetValidState__14CheckpointInfo(636, 0)
                scriptPtr += 1;
                if (Player::s_player) {
                    Player::s_player->checkpoint.SetValidState(0);
                }
                break;

            case DirectorOpcode::SpawnEffectFromMatrix:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;

                Humanoid* humanoid = FindHumanoidInHumanoidListByScriptRef(thingRef);
                if (!humanoid) {
                    scriptPtr += 1;
                    break;
                }

                HumanoidModel* model = humanoid->model ? static_cast<HumanoidModel*>(humanoid->model) : nullptr;
                AnimationMatrices* animMatrices = model ? model->animMatrices : nullptr;
                const s32* matrix = animMatrices ? animMatrices->GetMatrix(0) : nullptr;
                if (matrix) {
                    const LVector pos = { matrix[5], matrix[6], matrix[7] };
                    const s32 lifeFrames = *scriptPtr;
                    scriptPtr += 1;
                    GEffect_Create(kDirectorMatrixEffectHash, &pos, nullptr, nullptr, 0, lifeFrames, 0x80000001u);
                }
                break;
            }

            case DirectorOpcode::SpawnEffectFromAttack:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;

                Humanoid* humanoid = FindHumanoidInHumanoidListByScriptRef(thingRef);
                if (!humanoid || !humanoid->model) {
                    break;
                }

                HumanoidModel* model = static_cast<HumanoidModel*>(humanoid->model);
                AnimationMatrices* animMatrices = model->animMatrices;
                if (!animMatrices) {
                    break;
                }

                LVector prev = {};
                LVector cur = {};
                animMatrices->GetAttack(0u, prev, cur);
                GEffect_Create(kDirectorAttackEffectHash, &cur, nullptr, nullptr, 0, 0, 0x80000000u);
                break;
            }

            case DirectorOpcode::SpawnEffectAtPosA:
            {
                const LVector pos = { scriptPtr[1], scriptPtr[2], scriptPtr[3] };
                scriptPtr += 4;
                GEffect_Create(kDirectorAttackEffectHash, &pos, nullptr, nullptr, 0, 0, 0x80000000u);
                break;
            }

            case DirectorOpcode::SpawnEffectAtPosB:
            {
                const LVector pos = { scriptPtr[1], scriptPtr[2], scriptPtr[3] };
                scriptPtr += 4;
                GEffect_Create(kDirectorPosBEffectHash, &pos, nullptr, nullptr, 0, 25, 0);
                break;
            }

            case DirectorOpcode::SpawnEffectByUidAtPos:
            {
                const u32 effectHash = static_cast<u32>(scriptPtr[1]);
                const LVector pos = { scriptPtr[2], scriptPtr[3], scriptPtr[4] };
                scriptPtr += 5;

                GEffect_Create(effectHash, &pos, nullptr, nullptr, 0, 0, 0);
                break;
            }

            case DirectorOpcode::SpawnFwEffectAtPos:
            {
                const u32 effectHash = static_cast<u32>(scriptPtr[1]);
                const LVector pos = { scriptPtr[2], scriptPtr[3], scriptPtr[4] };
                scriptPtr += 5;

                FWEffect* fwEffect = FWEffect::Find(effectHash);
                if (fwEffect) {
                    const s32 blockNum = fwEffect->blockNum;
                    GEffect_Create(effectHash, &pos, nullptr, nullptr, 0, 0, 0);
                    fwEffect->blockNum = blockNum;
                }
                break;
            }

            case DirectorOpcode::DestroyDestructible:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;

                // PSX: FindNodeCRC(list=88 moveList, ref), call DestructibleThing::Destroy,
                // then set destroy flag (+128) and add UID to dead pool.
                Thing* thing = nullptr;
                if (g_ai) {
                    thing = FindThingInListByScriptRef(g_ai->moveList, thingRef);
                }

                DestructibleThing* destructible = dynamic_cast<DestructibleThing*>(thing);
                if (destructible) {
                    destructible->Destroy();
                    destructible->destroyFlag = 1;
                    levelDeadPool.AddUID(destructible->nameCRC);
                }
                break;
            }

            case DirectorOpcode::RemoveNisEffect:
                scriptPtr += 1;
                if (WEffect* effect = WEffect::Find(kDirectorNisEffectHash)) {
                    effect->pos.z -= 10000;
                    effect->NISRemoveEffect();
                }
                break;

            case DirectorOpcode::SetPlayerFlag:
            {
                // PSX: if scriptPtr[1] nonzero, set flag 4 on player; else clear it
                const s32 val = scriptPtr[1];
                scriptPtr += 2;
                if (Player::s_player) {
                    if (val)
                        Player::s_player->playerFlags |= PF_COMBAT_READY;
                    else
                        Player::s_player->playerFlags &= ~PF_COMBAT_READY;
                }
                break;
            }

            case DirectorOpcode::ClearGlobalEffectRef:
                scriptPtr += 1;
                // PSX case 0x29 writes 0x80000001 to gp+0xB8 = nisPointY (the same
                // global trio gfGoToVol fills), invalidating the NIS point Y. The
                // previous host translation wrote the player's groundStandHeight
                // instead, corrupting fall-damage ground tracking mid-NIS.
                nisPointY = static_cast<s32>(0x80000001u);
                break;

            case DirectorOpcode::DropPickup:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;
                Humanoid* humanoid = FindHumanoidByScriptRef(thingRef);
                if (humanoid) {
                    humanoid->DropPickup(1, 1);
                }
                break;
            }

            case DirectorOpcode::CameraFunc:
                scriptPtr += 1;
                ProcessCameraFunc();
                break;

            case DirectorOpcode::DoorFunc:
                scriptPtr += 1;
                ProcessDoorFunc();
                break;

            case DirectorOpcode::FacePointAndNisControl:
                // PSX: face player toward NIS point, write Behaviour::destPoint, set NisControl behaviour
                scriptPtr += 1;
                if (Player::s_player) {
                    const LVector target = { nisPointX, nisPointY, nisPointZ };
                    Player::s_player->FacePointDesired(target);
                    Behaviour* beh = Player::s_player->behaviour;
                    if (beh) {
                        beh->destPoint = target;
                        beh->handlerThisOffset = 0;
                        beh->handlerDispatch = -1;
                        beh->handler = Behaviour::NisControl;
                    }
                }
                break;

            case DirectorOpcode::LadderFunc:
                scriptPtr += 1;
                ProcessLadderFunc();
                break;

            case DirectorOpcode::ModelFunc:
                scriptPtr += 1;
                ProcessModelFunc();
                break;

            case DirectorOpcode::HudFunc:
                scriptPtr += 1;
                ProcessHudFunc();
                break;

            case DirectorOpcode::SetThingFlag08:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;

                // PSX: FindNodeCRC(list=52 humanoidList, ref)
                Thing* thing = nullptr;
                if (g_ai) {
                    thing = FindThingInListByScriptRef(g_ai->humanoidList, thingRef);
                }

                if (thing) {
                    thing->flags |= 0x8u;
                }
                break;
            }

            case DirectorOpcode::SetThingFlag28:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;

                // PSX: FindNodeCRC(list=52 humanoidList, ref)
                Thing* thing = nullptr;
                if (g_ai) {
                    thing = FindThingInListByScriptRef(g_ai->humanoidList, thingRef);
                }

                if (thing) {
                    thing->flags |= 0x28u;
                }
                break;
            }

            case DirectorOpcode::ClearThingFlagsAndKill:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;

                // PSX: FindNodeCRC(list=52 humanoidList, ref)
                Thing* thing = nullptr;
                if (g_ai) {
                    thing = FindThingInListByScriptRef(g_ai->humanoidList, thingRef);
                }

                if (thing) {
                    thing->flags &= ~0x28u;
                    thing->Kill();
                }
                break;
            }

            case DirectorOpcode::KillThingType52:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;

                // PSX: FindNodeCRC(list=52 humanoidList, ref)
                Thing* thing = nullptr;
                if (g_ai) {
                    thing = FindThingInListByScriptRef(g_ai->humanoidList, thingRef);
                }
                if (thing) {
                    thing->Kill();
                }
                break;
            }

            case DirectorOpcode::KillThingType88:
            {
                const u32 thingRef = static_cast<u32>(scriptPtr[1]);
                scriptPtr += 2;

                // PSX: FindNodeCRC(list=88 moveList, ref)
                Thing* thing = nullptr;
                if (g_ai) {
                    thing = FindThingInListByScriptRef(g_ai->moveList, thingRef);
                }

                if (thing) {
                    thing->Kill();
                }
                break;
            }

            case DirectorOpcode::KillThingsIfCombat:
                scriptPtr += 1;
                // PSX DIRECTOR.CPP:3520..3527
                // Reads player blockNum (Thing +84), computes blockNum-5, and only
                // calls AI::KillThings(arg) when arg > 0.
                if (g_ai && Player::s_player) {
                    const s32 killArg = static_cast<s32>(Player::s_player->blockNum) - 5;
                    if (killArg > 0) {
                        g_ai->KillThings(killArg);
                    }
                }
                break;

            case DirectorOpcode::HumanoidFunc:
                scriptPtr += 1;
                ProcessHumanoidFunc();
                break;

            case DirectorOpcode::ResetCameraManager:
                // PSX: calls cameraManager vtable func (InternalReset)
                scriptPtr += 1;
                if (g_cameraManager) {
                    g_cameraManager->Reset();
                }
                break;

            case DirectorOpcode::SetDesiredWideScreen:
                SetDesiredWideScreen();
                break;

            case DirectorOpcode::ResetWideScreenDefaults:
                // PSX: reset to defaults and hide HUD
                wsBarTarget = 120;
                wsBarStep = 256;
                wsAlphaTarget = 255;
                wsAlphaStep = 10;
                wsMode = 2;
                scriptPtr += 1;
                if (g_hud) {
                    g_hud->SetHUDVisible(0, 0);
                }
                break;

            case DirectorOpcode::SetSoundScript:
                // PSX: creates CDirectorSound (type 10140) if not yet created
                if (!directorSound) {
                    directorSound = new CDirectorSound();
                    directorSound->Initialize();
                }
                scriptPtr += 2;
                scriptBase = ScriptPtrFromWord(scriptPtr[-1]);
                RegisterRuntimeScriptRegion(scriptBase);
                break;

            case DirectorOpcode::EdisonFunc:
                scriptPtr += 1;
                ProcessEdison();
                break;

            case DirectorOpcode::SoundCdYield:
                scriptPtr += 1;
                rsEvent(12, 6, 0, 0);
                break;

            case DirectorOpcode::SoundCdAccess:
                scriptPtr += 1;
                rsEvent(13, 6, 0, 0);
                break;

            case DirectorOpcode::LoadDialogA:
            {
                const s32 character = scriptPtr[1];
                const s32 dialogId = scriptPtr[2];
                g_dialogHandle = rsEvent(RS_LOAD_DIALOG, character, dialogId, 0x40000100);
                directorDialogCounter = 0;
                scriptPtr += 3;
                break;
            }

            case DirectorOpcode::LoadDialogB:
            {
                const s32 character = scriptPtr[1];
                const s32 dialogId = scriptPtr[2];
                const s32 priority = scriptPtr[3];
                g_dialogHandle = rsEvent(RS_LOAD_DIALOG, character, dialogId, priority);
                directorDialogCounter = 0;
                scriptPtr += 4;
                break;
            }

            case DirectorOpcode::WaitDialogPlayable:
            {
                directorDialogCounter++;
                const s32 handle = g_dialogHandle;
                const s32 valid = (handle != 0 && jcsValidateHandle(handle)) ? 1 : 0;
                s32 playable = -1;

                if (valid && directorDialogCounter < directorDialogLimit) {
                    playable = jcsIsPlayable(handle);
                    if (!playable) {
                        field68 = 1;
                        break;
                    }
                }
                field68 = 0;
                scriptPtr += 1;
                break;
            }

            case DirectorOpcode::PlayDialogNear:
            {
                const s32 handle = g_dialogHandle;
                const s32 valid = (handle != 0 && jcsValidateHandle(handle)) ? 1 : 0;
                s32 started = 0;
                if (valid) {
                    SetPendingDialogPlayPosition(nullptr);
                    started = rsEvent(RS_PLAY_DIALOG, handle, 0, 64);
                }
                scriptPtr += 1;
                break;
            }

            case DirectorOpcode::PlayDialogFar:
            {
                const s32 handle = g_dialogHandle;
                const s32 valid = (handle != 0 && jcsValidateHandle(handle)) ? 1 : 0;
                s32 started = 0;
                if (valid) {
                    SetPendingDialogPlayPosition(nullptr);
                    started = rsEvent(RS_PLAY_DIALOG, handle, 0, 100);
                }
                scriptPtr += 1;
                break;
            }

            case DirectorOpcode::PlayPriorityDialog:
                if (Player::s_player) {
                    Player::s_player->PlayDialogBasedOnPriority(255, 1073742080);
                }
                scriptPtr += 1;
                break;

            case DirectorOpcode::SetDialogTimeout:
                directorDialogLimit = scriptPtr[1];
                scriptPtr += 2;
                break;

            case DirectorOpcode::SetNisPoint:
            {
                // PSX: reads point CRC, looks up WorldPoints, stores director NIS point,
                // and immediately copies it to player pos/homePos.
                const s32 pointIdx = scriptPtr[1];
                scriptPtr += 2;
                WorldPointNode* wpn = WorldPoints_GetNISPoint(static_cast<u32>(pointIdx));
                if (wpn) {
                    nisPointX = wpn->pos.x;
                    nisPointY = wpn->pos.y;
                    nisPointZ = wpn->pos.z;

                    if (Player::s_player) {
                        Player::s_player->pos = wpn->pos;
                        Player::s_player->homePos = wpn->pos;
                        if (wpn->parValue != 9999) {
                            Player::s_player->blockNum = static_cast<u16>(wpn->parValue);
                        }
                        Player::s_player->ResetRenderInterpolation();
                    }
                }
                break;
            }

            case DirectorOpcode::SetGotoCheckpoint:
            {
                scriptPtr += 1;

                char gotoPointName[32];
                const char petalLetter = static_cast<char>(g_checkpointPetalIndex + 'a');
                std::snprintf(gotoPointName, sizeof(gotoPointName), "goto%c%d", petalLetter, g_checkpointBlockIndex);

                if (!setJackieCheckpoint(p3dHash(gotoPointName))) {
                    scriptPtr = ResolveDirectorScriptVA(kDirectorScriptDefaultBegin);
                }

                if (Player::s_player) {
                    Player::s_player->SetLivesLeft(Player::s_player->GetLivesLeft() + 1);
                }
                break;
            }

            case DirectorOpcode::SetFallbackCheckpoint:
                scriptPtr += 1;

                if (!setJackieCheckpoint(kDirectorFallbackCheckpointHash)) {
                    scriptPtr = ResolveDirectorScriptVA(kDirectorScriptDefaultBegin);
                }

                if (Player::s_player) {
                    Player::s_player->SetLivesLeft(Player::s_player->GetLivesLeft() + 1);
                }
                levelDeadPool.InternalReset();
                break;

            case DirectorOpcode::SetBlockCheckpoint:
            {
                const s32 blockNum = g_checkpointBlockIndex;
                scriptPtr += 1;

                Block* block = g_blockManager ? g_blockManager->GetBlock(static_cast<u32>(blockNum)) : nullptr;
                if (block) {
                    LVector checkpointPos = { block->posX, block->posY, block->posZ };
                    checkpointPos.y += block->dimY / 2;
                    checkPoint(checkpointPos, blockNum);

                    if (Player::s_player) {
                        Player::s_player->SetLivesLeft(Player::s_player->GetLivesLeft() + 1);
                    }
                }
                else {
                    scriptPtr = ResolveDirectorScriptVA(kDirectorScriptDefaultBegin);
                }
                break;
            }

            case DirectorOpcode::DetermineVictory:
            {
                scriptPtr += 1;

                DetermineVictoryIdle();
                break;
            }

            case DirectorOpcode::DetermineDeath:
                scriptPtr += 1;
                DetermineDeath();
                break;

            default:
                // PSX: unrecognised opcodes fall to the loop-top (def_8003C430) without
                // advancing scriptPtr — the script blocks on the bad opcode each frame
                // rather than desyncing by consuming one word.
                LOG("[Director] unknown opcode 0x%02X at scriptPtr=%p", static_cast<u32>(opcode), static_cast<const void*>(scriptPtr));
                field68 = 1;
                break;
        }

        // A handler may cause a collision callback which replaces the script via
        // SetCodeSnip. Its normal pointer advance then applies to the new script.
        // Restart that snippet on the next frame instead of consuming its prefix.
        if (codeSnipPtr != codeSnipBeforeDispatch) {
            scriptPtr = codeSnipPtr;
            return;
        }
    }

    field68 = 1;
}

// PSX: ProcessSoundScript__8Director (DIRECTOR.CPP:3576, 0x8003D5A4)
void Director::ProcessSoundScript() {
    MARKFUNCTION(0x8003D5A4);

    const s32 elapsed = GetDirectorFrameCounter() - timerStart;

    while (scriptBase) {
        const u32 packed = static_cast<u32>(*scriptBase);
        const s32 eventFrame = static_cast<s32>(packed & 0xFFF);
        if (packed == 0 || eventFrame >= elapsed) {
            break;
        }

        scriptBase += 1;

        // PSX: ProcessNISEvent(field192, packed >> 28, packed >> 12)
        if (directorSound) {
            const u32 eventType = packed >> 28;
            const u16 soundId = static_cast<u16>(packed >> 12);
            directorSound->ProcessNISEvent(eventType, soundId);
        }
    }
}

// PSX: Timer__8Director (DIRECTOR.CPP:3611, 0x8003D634)
// Returns 1 when timer done/reset, 0 when blocked (still waiting).
s32 Director::TimerStep() {
    MARKFUNCTION(0x8003D634);

    if (!scriptPtr) {
        return 0;
    }

    const s32 duration = scriptPtr[1];
    if (duration == -1) {
        timerTarget = 0;
        timerStart = GetDirectorFrameCounter();
        scriptPtr += 2;
        return 1;
    }

    if (timerTarget) {
        // Timer already running - check if expired
        if (static_cast<s32>(GetDirectorFrameCounter()) >= timerTarget) {
            scriptPtr += 2;
            timerTarget = 0;
            return 1;
        }
        return 0;
    }

    // First frame - set target
    timerTarget = timerStart + duration;
    return 0;
}

// PSX: Loop__8Director (DIRECTOR.CPP:3637, 0x8003D6CC)
void Director::Loop() {
    MARKFUNCTION(0x8003D6CC);
}

// PSX: SetDesiredWideScreen__8Director (DIRECTOR.CPP:3642, 0x8003D6D4)
void Director::SetDesiredWideScreen() {
    MARKFUNCTION(0x8003D6D4);

    if (!scriptPtr) {
        return;
    }

    scriptPtr += 1;

    for (s32 i = 0; i < 64; i++) {
        if (!scriptPtr) {
            return;
        }

        const DirectorWideScreenCmd token = static_cast<DirectorWideScreenCmd>(*scriptPtr);
        if (token == DirectorWideScreenCmd::End) {
            scriptPtr += 1;
            return;
        }

        scriptPtr += 1;

        switch (token) {
            case DirectorWideScreenCmd::SetAlphaTarget:
                wsAlphaTarget = *scriptPtr;
                scriptPtr += 1;
                break;

            case DirectorWideScreenCmd::SetAlphaCurrent:
                wsAlphaCurrent = *scriptPtr;
                scriptPtr += 1;
                break;

            case DirectorWideScreenCmd::SetAlphaStep:
                wsAlphaStep = *scriptPtr;
                scriptPtr += 1;
                break;

            case DirectorWideScreenCmd::SetBarTarget:
                wsBarTarget = *scriptPtr;
                scriptPtr += 1;
                break;

            case DirectorWideScreenCmd::SetBarStep:
                wsBarStep = *scriptPtr;
                scriptPtr += 1;
                break;

            case DirectorWideScreenCmd::SetMode:
            {
                s16* p = reinterpret_cast<s16*>(scriptPtr);
                wsMode = static_cast<u16>(*p);
                p += 2;
                scriptPtr = reinterpret_cast<s32*>(p);
                break;
            }

            default:
                // PSX: unknown widescreen tokens are ignored.
                break;
        }
    }

    scriptPtr = nullptr;
}

// PSX: ProcessEdison__8Director (DIRECTOR.CPP:3689, 0x8003D800)
void Director::ProcessEdison() {
    MARKFUNCTION(0x8003D800);

    if (!scriptPtr) {
        return;
    }

    const DirectorEdisonCmd opcode = static_cast<DirectorEdisonCmd>(*scriptPtr);
    scriptPtr += 1;

    if (opcode == DirectorEdisonCmd::PlayTransient) {
        // PSX: reads u16 soundID, advances scriptPtr, calls CSoundDirect::PlayTransient(soundID, 0, 0, 0)
        s16* soundData = reinterpret_cast<s16*>(scriptPtr);
        const u16 soundID = static_cast<u16>(*soundData);
        soundData += 2;
        scriptPtr = reinterpret_cast<s32*>(soundData);

        // PSX: PlayTransient__12CSoundDirectUsPC10tagLVectorUsUl(soundID, 0, 0, 0)
        CSoundDirect::PlayTransient(soundID, nullptr, 0, 0);
    }
    else if (opcode == DirectorEdisonCmd::StopMusic) {
        rsEvent(RS_STOP_MUSIC, 0, 0, 0);
    }
    // PSX: unknown Edison opcodes are ignored.
}

// PSX: ProcessModelFunc__8Director (DIRECTOR.CPP:3711, 0x8003D87C)
void Director::ProcessModelFunc() {
    MARKFUNCTION(0x8003D87C);
}

// PSX: ProcessCameraFunc__8Director (DIRECTOR.CPP:3716, 0x8003D884)
void Director::ProcessCameraFunc() {
    MARKFUNCTION(0x8003D884);

    if (!scriptPtr) {
        return;
    }

    const DirectorCameraCmd op = static_cast<DirectorCameraCmd>(*scriptPtr);
    scriptPtr += 1;

    Camera* camera = GetGameCamera();

    switch (op) {
        case DirectorCameraCmd::EnableNisCamera:
            if (camera) {
                // PSX: lookAtMode = 1; then dispatch Camera::Think() via vtable slot.
                camera->SetLookAtMode(1);
                camera->Think();
            }
            break;

        case DirectorCameraCmd::ClearCameraFlag:
            if (camera) {
                camera->SetCollisionEnabled(0);
            }
            break;

        case DirectorCameraCmd::SetCameraFlag:
            if (camera) {
                camera->SetCollisionEnabled(1);
            }
            break;

        case DirectorCameraCmd::CopyP3DFov:
            if (camera) {
                // PSX: GetFOV from global G_2ptcam, then SetFOV(rmDiv16i(fovA, 87162)).
                s32 fovA, fovB;
                G_2ptcam.GetFOV(&fovA, &fovB);
                camera->SetFOV(rmDiv16i(fovA, 87162));
            }
            break;

        case DirectorCameraCmd::ResetCameraFov:
            if (camera) {
                // PSX: SetCurFOV(theCamera->curFOV)
                camera->SetCurFOV(camera->GetCurFOV());
            }
            break;

        case DirectorCameraCmd::SetCameraMode:
        {
            const s32 mode = *scriptPtr;
            scriptPtr += 1;
            // PSX DIRECTOR.CPP:3781..3783 forwards the raw script value.
            if (camera) {
                camera->SetMode(static_cast<CameraMode>(mode));
            }
            break;
        }

        case DirectorCameraCmd::LoadAsyncAnim:
            if (camera) {
                camera->LoadAsyncAnim(*scriptPtr);
            }
            scriptPtr += 1;
            break;

        case DirectorCameraCmd::DeleteAsyncAnim:
            if (camera) {
                camera->DeleteAsyncAnim();
            }
            break;

        case DirectorCameraCmd::PlayAsyncAnim:
            if (camera) {
                camera->PlayAsyncAnim();
            }
            break;

        case DirectorCameraCmd::ShakeCamera:
        {
            const s32 selector = *scriptPtr;
            scriptPtr += 1;

            s32 shakeX = 80;
            s32 shakeY = 80;
            s32 shakeZ = 80;

            if (selector == 0) {
                shakeX = *scriptPtr;
                scriptPtr += 1;
            }
            else if (selector == 1) {
                shakeY = *scriptPtr;
                scriptPtr += 1;
            }
            else {
                // PSX treats any non-0/1 selector as Z strength.
                shakeZ = *scriptPtr;
                scriptPtr += 1;
            }

            const s32 frames = *scriptPtr;
            scriptPtr += 1;

            if (camera) {
                camera->SetShakeStrength(shakeX, shakeY, shakeZ);
                camera->ShakeCamera(frames);
            }
            break;
        }

        case DirectorCameraCmd::LookAtNisPoint:
        {
            if (camera) {
                camera->SetCollisionEnabled(0);
            }

            const s32 pointID = *scriptPtr;
            scriptPtr += 1;

            WorldPointNode* nisPoint = WorldPoints_GetNISPoint(static_cast<u32>(pointID));

            if (camera && nisPoint) {
                LVector target = nisPoint->pos;

                World* world = g_game ? g_game->GetWorld() : nullptr;
                const bool isChinaLevel2 = (world && world->GetCurLevelID() == 3 && world->GetCurrentPetalIndex() == 2);
                if (isChinaLevel2) {
                    target.z -= kLookAtNisPointOffsetXZ;
                }
                else {
                    target.x -= kLookAtNisPointOffsetXZ;
                    target.y -= kLookAtNisPointOffsetY;
                }

                camera->LookAtTarget(&target);
            }

            break;
        }

        case DirectorCameraCmd::SetCameraAndLookAt:
        {
            if (camera) {
                camera->SetCollisionEnabled(0);
            }

            const LVector position = { scriptPtr[0], scriptPtr[1], scriptPtr[2] };
            const LVector target = { scriptPtr[3], scriptPtr[4], scriptPtr[5] };
            scriptPtr += 6;

            if (camera) {
                camera->SetPositionAndPrev(position.x, position.y, position.z);
                camera->LookAtTarget(&target);
            }
            break;
        }

        default:
            // PSX default path just returns for unknown camera tokens.
            return;
    }
}

// PSX: ProcessHudFunc__8Director (DIRECTOR.CPP:3845, 0x8003DC44)
void Director::ProcessHudFunc() {
    MARKFUNCTION(0x8003DC44);

    if (!scriptPtr) {
        return;
    }

    const DirectorHudCmd op = static_cast<DirectorHudCmd>(*scriptPtr);
    scriptPtr += 1;

    switch (op) {
        case DirectorHudCmd::HideHud:
            if (g_hud) {
                g_hud->SetHUDVisible(0, 0);
            }
            break;

        case DirectorHudCmd::ShowHud:
            if (g_hud) {
                g_hud->SetHUDVisible(1, 1);
            }
            break;

        case DirectorHudCmd::DisplayTally:
        {
            const s32 tallyType = *scriptPtr;
            scriptPtr += 1;
            if (g_hud) {
                g_hud->DisplayTally(tallyType);
            }
            break;
        }

        case DirectorHudCmd::ShowBossHealth:
        {
            const u32 nameArg = static_cast<u32>(*scriptPtr);
            scriptPtr += 1;

            if (!g_hud || nameArg == 0) {
                break;
            }

            const char* name = ResolveHudBossNamePtr(nameArg);
            if (!name) {
                LOG("[Director] unresolved ShowBossHealth arg 0x%08X", nameArg);
                break;
            }

            g_hud->ShowBossHealth(name);
            break;
        }

        default:
            // PSX: unknown HUD tokens are ignored.
            break;
    }
}

// PSX: ProcessHumanoidFunc__8Director (DIRECTOR.CPP:3894, 0x8003DD10)
void Director::ProcessHumanoidFunc() {
    MARKFUNCTION(0x8003DD10);

    if (!scriptPtr) {
        return;
    }

    const u32 thingRef = static_cast<u32>(*scriptPtr);
    scriptPtr += 1;

    Humanoid* humanoid = FindHumanoidInHumanoidListByScriptRef(thingRef);

    for (s32 i = 0; i < 128; i++) {
        if (!scriptPtr) {
            return;
        }

        const DirectorHumanoidCmd op = static_cast<DirectorHumanoidCmd>(*scriptPtr);
        if (op == DirectorHumanoidCmd::End) {
            scriptPtr += 1;
            return;
        }

        scriptPtr += 1;

        switch (op) {
            case DirectorHumanoidCmd::EnterNis:
                if (humanoid) {
                    const u32 flags2 = static_cast<u32>(humanoid->flags2);
                    if (((flags2 >> 4) & 1u) == 0) {
                        humanoid->flags2 = static_cast<s32>(flags2 | 0x30u);
                        humanoid->field516 = 0;
                        humanoid->field520 = 0;
                        humanoid->field524 = 0;
                    }
                }
                break;

            case DirectorHumanoidCmd::EnterNisMove:
                if (humanoid) {
                    const u32 flags2 = static_cast<u32>(humanoid->flags2);
                    if (((flags2 >> 4) & 1u) == 0 || ((((flags2 >> 5) & 1u) != 0) && (((flags2 >> 6) & 1u) != 0))) {
                        humanoid->flags2 = static_cast<s32>((flags2 | 0x10u) & ~0x60u);
                        humanoid->field516 = 0;
                        humanoid->field520 = 0;
                        humanoid->field524 = 0;
                    }
                }
                break;

            case DirectorHumanoidCmd::ExitNis:
                if (humanoid) {
                    humanoid->flags2 &= ~0x70u;
                }
                break;

            case DirectorHumanoidCmd::FaceAngleDegrees:
            {
                const s32 deg = *scriptPtr;
                scriptPtr += 1;
                const s32 angle = (deg << 16) / 360;
                if (humanoid) {
                    humanoid->orientation.y = angle;
                    humanoid->SetDesiredMoveDirection(angle);
                    humanoid->FaceAngleY(angle, 0);
                }
                break;
            }

            case DirectorHumanoidCmd::StandFacingZero:
                if (humanoid) {
                    humanoid->flags2 &= ~0x70u;
                    humanoid->SetActionState(AS_NIS_MODE, 0);
                    humanoid->flags &= ~0x800u;
                    humanoid->orientation.y = 0;
                    humanoid->SetDesiredMoveDirection(0);
                    humanoid->FaceAngleY(0, 0);

                    const u32 flags2 = static_cast<u32>(humanoid->flags2);
                    if (((flags2 >> 4) & 1u) == 0 || ((((flags2 >> 5) & 1u) != 0) && (((flags2 >> 6) & 1u) != 0))) {
                        humanoid->flags2 = static_cast<s32>((flags2 | 0x10u) & ~0x60u);
                        humanoid->field516 = 0;
                        humanoid->field520 = 0;
                        humanoid->field524 = 0;
                    }

                    humanoid->flags |= 8u;
                }
                break;

            case DirectorHumanoidCmd::SetPosition:
            {
                const LVector position = { scriptPtr[0], scriptPtr[1], scriptPtr[2] };
                scriptPtr += 3;
                if (humanoid) {
                    humanoid->pos = position;
                    humanoid->homePos = position;
                    humanoid->ResetRenderInterpolation();
                }
                break;
            }

            case DirectorHumanoidCmd::PlayDynamicAnim:
            {
                const s32 animEnumVal = *scriptPtr;
                scriptPtr += 1;
                if (humanoid && humanoid->model) {
                    SModel* sm = static_cast<SModel*>(humanoid->model);
                    sm->PlayDynamicAnim(animEnumVal);
                }
                break;
            }

            case DirectorHumanoidCmd::SetStandState:
                if (humanoid) {
                    humanoid->SetActionState(AS_NIS_MODE, 0);
                    humanoid->flags &= ~0x800u;
                }
                break;

            case DirectorHumanoidCmd::SetPositionByCurrent:
                // PSX DIRECTOR.CPP:3987 writes pos/homePos directly (pos.y - 400).
                // It does NOT reset velocity or re-run CheckThingSwitches, so this
                // must not call Teleport (which would re-fire death volumes mid-NIS).
                if (humanoid) {
                    LVector position = humanoid->pos;
                    position.y -= 400;
                    humanoid->pos = position;
                    humanoid->homePos = position;
                    humanoid->ResetRenderInterpolation();
                }
                break;

            default:
                continue;
        }
    }

    scriptPtr = nullptr;
}

// PSX: ProcessLadderFunc__8Director (DIRECTOR.CPP:4003, 0x8003E0D4)
void Director::ProcessLadderFunc() {
    MARKFUNCTION(0x8003E0D4);

    if (!scriptPtr) {
        return;
    }

    for (s32 i = 0; i < 128; i++) {
        const DirectorLadderCmd op = static_cast<DirectorLadderCmd>(*scriptPtr);
        if (op == DirectorLadderCmd::End) {
            scriptPtr += 1;
            return;
        }

        scriptPtr += 1;

        switch (op) {
            case DirectorLadderCmd::FaceLadderPoint:
            {
                const s32 axis = *scriptPtr;
                scriptPtr += 1;

                if (Player::s_player) {
                    LVector target = Player::s_player->pos;
                    if (axis == 1 && GetCurrentWorldLevelID() == 3) {
                        target.x += kLadderFacePointOffset;
                    }
                    else {
                        target.z += kLadderFacePointOffset;
                    }
                    Player::s_player->FacePointDesired(target);
                    if (Player::s_player->behaviour) {
                        Player::s_player->behaviour->destPoint = target;
                        Player::s_player->behaviour->handlerThisOffset = 0;
                        Player::s_player->behaviour->handlerDispatch = -1;
                        Player::s_player->behaviour->handler = Behaviour::NisControl;
                    }
                }
                break;
            }

            case DirectorLadderCmd::TeleportPlayer:
            {
                Ladder* ladder = dynamic_cast<Ladder*>(g_selectedDoorOrLadder);
                if (ladder) {
                    ladder->TeleportPlayer();
                }
                break;
            }

            case DirectorLadderCmd::CameraLookAtHatch:
                if (Player::s_player) {
                    LVector normal = { 0, 0, -65536 };
                    LVector latchPos = Player::s_player->pos;
                    latchPos.y += 1536;
                    latchPos.z += 512;
                    latchPos.y = g_collisionSectors[0].GetWorldFloorHeight(latchPos, 0);
                    latchPos.z -= 384;

                    Player::s_player->PrepareLedgeLatch(latchPos, normal);
                    Player::s_player->SetActionState(AS_LEDGE_PULLUP, 0);
                }
                break;

            case DirectorLadderCmd::CloseHatch:
            {
                Ladder* ladder = dynamic_cast<Ladder*>(g_selectedDoorOrLadder);
                if (ladder) {
                    ladder->CloseHatch();
                }
                break;
            }

            case DirectorLadderCmd::ClearNis:
                if (Player::s_player) {
                    Camera* camera = GetGameCamera();
                    if (camera) {
                        Player::s_player->FacePointDesired(camera->GetPosition());
                    }
                    if (Player::s_player->behaviour) {
                        Player::s_player->behaviour->handlerThisOffset = 0;
                        Player::s_player->behaviour->handlerDispatch = 0;
                        Player::s_player->behaviour->handler = nullptr;
                    }
                }
                break;

            default:
                continue;
        }
    }

    scriptPtr = nullptr;
}

// PSX: ProcessDoorFunc__8Director (DIRECTOR.CPP:4069, 0x8003E378)
void Director::ProcessDoorFunc() {
    MARKFUNCTION(0x8003E378);

    if (!scriptPtr) {
        return;
    }

    if (!g_selectedDoorOrLadder && g_codeSnipThing) {
        g_selectedDoorOrLadder = g_codeSnipThing;
    }

    for (s32 i = 0; i < 128; i++) {
        const DirectorDoorCmd op = static_cast<DirectorDoorCmd>(*scriptPtr);

        if (op == DirectorDoorCmd::End) {
            scriptPtr += 1;
            return;
        }

        scriptPtr += 1;

        switch (op) {
            case DirectorDoorCmd::SetDoor:
            {
                const u32 thingRef = static_cast<u32>(*scriptPtr);
                scriptPtr += 1;

                Thing* thing = nullptr;
                if (g_ai) {
                    ccNode* n = g_ai->moveList.FindNodeCRC(thingRef);
                    if (n) {
                        thing = static_cast<Thing*>(n);
                    }
                }

                if (thing) {
                    g_selectedDoorOrLadder = thing;
                }
                break;
            }

            case DirectorDoorCmd::OpenDoor:
            {
                Door* door = dynamic_cast<Door*>(g_selectedDoorOrLadder);
                if (door) {
                    door->Open();
                }
                break;
            }

            case DirectorDoorCmd::SetDoorState:
            {
                Door* door = dynamic_cast<Door*>(g_selectedDoorOrLadder);
                if (door) {
                    door->doorState = 4;
                }
                break;
            }

            case DirectorDoorCmd::FaceDoorPoint:
            {
                const u32 thingRef = static_cast<u32>(*scriptPtr);
                scriptPtr += 1;

                Humanoid* humanoid = FindHumanoidByScriptRef(thingRef);
                Door* door = dynamic_cast<Door*>(g_selectedDoorOrLadder);
                if (humanoid && door) {
                    // PSX DIRECTOR.CPP:4093..4099 uses the raw collBox midpoint.
                    LVector boxCenter = {};
                    boxCenter.x = ((s32)door->collBox.minX + (s32)door->collBox.maxX) / 2;
                    boxCenter.y = ((s32)door->collBox.minY + (s32)door->collBox.maxY) / 2;
                    boxCenter.z = ((s32)door->collBox.minZ + (s32)door->collBox.maxZ) / 2;
                    humanoid->FacePointDesired(boxCenter);
                }
                break;
            }

            case DirectorDoorCmd::FaceDoorAngle:
            {
                const u32 thingRef = static_cast<u32>(*scriptPtr);
                scriptPtr += 1;

                Humanoid* humanoid = FindHumanoidByScriptRef(thingRef);
                Door* door = dynamic_cast<Door*>(g_selectedDoorOrLadder);
                if (humanoid && door) {
                    humanoid->FaceAngleY(door->orientation.y, 1);
                }
                break;
            }

            case DirectorDoorCmd::AttachToDoor:
            {
                const u32 thingRef = static_cast<u32>(*scriptPtr);
                scriptPtr += 1;

                Humanoid* humanoid = FindHumanoidByScriptRef(thingRef);
                Door* door = dynamic_cast<Door*>(g_selectedDoorOrLadder);
                if (humanoid && door && humanoid->behaviour) {
                    LVector localCenter = {};
                    localCenter.x = ((s32)door->collBox.minX + (s32)door->collBox.maxX) / 2;
                    localCenter.y = ((s32)door->collBox.minY + (s32)door->collBox.maxY) / 2;
                    localCenter.z = ((s32)door->collBox.minZ + (s32)door->collBox.maxZ) / 2;

                    s32 sinY = rmSin16(door->orientation.y);
                    s32 cosY = rmSin16(door->orientation.y + 0x4000);

                    LVector attachPos = {};
                    attachPos.x = door->pos.x + (s32)(((s64)cosY * localCenter.x) >> 16) + (s32)(((s64)sinY * localCenter.z) >> 16);
                    attachPos.y = door->pos.y;
                    attachPos.z = door->pos.z + (s32)((-(s64)sinY * localCenter.x) >> 16) + (s32)(((s64)cosY * localCenter.z) >> 16);

                    humanoid->behaviour->destPoint = attachPos;
                    humanoid->behaviour->handlerThisOffset = 0;
                    humanoid->behaviour->handlerDispatch = -1;
                    humanoid->behaviour->handler = Behaviour::NisControl;
                }
                break;
            }

            case DirectorDoorCmd::TeleportThroughDoor:
            {
                Door* door = dynamic_cast<Door*>(g_selectedDoorOrLadder);
                if (door) {
                    door->TeleportPlayer();
                    door->Reset();
                }
                break;
            }

            default:
                // PSX: unknown door tokens are ignored.
                break;
        }
    }

    scriptPtr = nullptr;
}

// PSX: DetermineVictoryIdle__8Director (DIRECTOR.CPP:4170, 0x8003E71C)
void Director::DetermineVictoryIdle() {
    MARKFUNCTION(0x8003E71C);

    s32* victoryScript = ResolveDirectorScriptVA(kDirectorScriptVictoryPoor);

    if (victoryBossCRC == kHashDante) {
        victoryScript = ResolveDirectorScriptVA(kDirectorScriptWaitSubroutine);
    }
    else if (victoryBossCRC == kHashDisco) {
        victoryScript = ResolveDirectorScriptVA(kDirectorScriptVictoryDisco);
    }
    else if (victoryBossCRC == kHashBarney) {
        victoryScript = ResolveDirectorScriptVA(kDirectorScriptVictoryGrontar);
    }
    else if (victoryBossCRC == kHashChef) {
        victoryScript = ResolveDirectorScriptVA(kDirectorScriptVictoryChef);
    }
    else if (victoryBossCRC == kHashClown) {
        victoryScript = ResolveDirectorScriptVA(kDirectorScriptVictoryClown);
    }
    else {
        // PSX: rating = GetLevelEndRating__12ScoreManager(theScoreManager)
        s32 rating = 0;
        if (g_scoreManager) {
            rating = g_scoreManager->GetLevelEndRating();
        }
        switch (rating) {
            case 0:
                victoryScript = ResolveDirectorScriptVA(kDirectorScriptVictoryPoor);
                break;
            case 1:
                victoryScript = ResolveDirectorScriptVA(kDirectorScriptVictoryOk);
                break;
            case 2:
                victoryScript = ResolveDirectorScriptVA(kDirectorScriptVictoryGood);
                break;
            case 3:
                victoryScript = ResolveDirectorScriptVA(kDirectorScriptVictoryPerfect);
                break;
            default:
                victoryScript = ResolveDirectorScriptVA(kDirectorScriptVictoryPoor);
                break;
        }
    }

    PushScriptReturnAddress(this, victoryScript);
}

// PSX: DetermineLevelIntro__8Director (DIRECTOR.CPP:4260, 0x8003E864)
void Director::DetermineLevelIntro() {
    MARKFUNCTION(0x8003E864);

    s32* introScript = ResolveDirectorScriptVA(kDirectorScriptStartGeneric);
    s32 levelID = GetCurrentWorldLevelID();

    if (levelID == 7) {
        visitedLevels = 0;
    }

    // PSX: if (IsValid__14CheckpointInfo(player+636) && checkpoint_killCount > 0)
    //          KillThings__2AIl(0, checkpoint_killCount);
    if (Player::s_player && Player::s_player->checkpoint.IsValid()) {
        if (Player::s_player->checkpoint.field28 > 0) {
            g_ai->KillThings(Player::s_player->checkpoint.field28);
        }
    }

    const bool visitedLevel = (levelID >= 0 && levelID < 31) ? ((visitedLevels & (1 << levelID)) != 0) : false;
    if (visitedLevel) {
        g_directorActive = 0;

        switch (levelID) {
            case 8:
            {
                introScript = ResolveDirectorScriptVA(kDirectorScriptIntroAllDante);
                break;
            }

            case 11:
            {
                introScript = ResolveDirectorScriptVA(kDirectorScriptIntroEveryChef);
                break;
            }

            case 12:
            {
                introScript = ResolveDirectorScriptVA(kDirectorScriptIntroEveryGrontar);
                break;
            }

            case 13:
            {
                introScript = ResolveDirectorScriptVA(kDirectorScriptIntroEveryClown);
                break;
            }

            case 14:
            {
                introScript = ResolveDirectorScriptVA(kDirectorScriptIntroEveryDisco);
                break;
            }

            default:
                // Checkpoint respawn on a revisited generic level: skip the
                // 45-tick widescreen-bar wait. On PSX that timer completed during
                // the slow CD load, so input was ready the moment the game appeared.
                // On PC the load is instant, so we use a minimal intro instead.
                if (Player::s_player && Player::s_player->checkpoint.IsValid()) {
                    introScript = start_generic_checkpoint;
                }
                break;
        }
    }
    else {
        switch (levelID) {
            case 1:
                if (g_game && g_game->GetWorld() && g_game->GetWorld()->GetCurrentPetalIndex() == 0) {
                    introScript = ResolveDirectorScriptVA(kDirectorScriptStartFrantic);
                }
                else {
                    g_directorActive = 0;
                    introScript = ResolveDirectorScriptVA(kDirectorScriptStartGeneric);
                }
                break;

            case 8:
                g_directorActive = 0;
                {
                    introScript = ResolveDirectorScriptVA(kDirectorScriptIntroAllDante);
                    break;
                }

            case 11:
            {
                introScript = ResolveDirectorScriptVA(kDirectorScriptIntroChef);
                break;
            }

            case 12:
            {
                introScript = ResolveDirectorScriptVA(kDirectorScriptIntroGrontar);
                break;
            }

            case 13:
            {
                introScript = ResolveDirectorScriptVA(kDirectorScriptIntroClown);
                break;
            }

            case 14:
            {
                introScript = ResolveDirectorScriptVA(kDirectorScriptIntroDisco);
                break;
            }

            default:
                g_directorActive = 0;
                introScript = ResolveDirectorScriptVA(kDirectorScriptStartGeneric);
                break;
        }
    }

    PushScriptReturnAddress(this, introScript);
}

// PSX: DetermineDeath__8Director (DIRECTOR.CPP:4382, 0x8003EA4C)
void Director::DetermineDeath() {
    MARKFUNCTION(0x8003EA4C);

    s32* deathScript = ResolveDirectorScriptVA(kDirectorScriptDeathGeneric);
    const s32 levelID = GetCurrentWorldLevelID();

    if (deathType <= 0) {
        if (levelID < 2 || levelID >= 4) {
            deathScript = ResolveDirectorScriptVA(kDirectorScriptDeathFallPavement);
        }
        else {
            deathScript = ResolveDirectorScriptVA(kDirectorScriptDeathFallWater);
        }
    }
    else {
        switch (deathType) {
            case 1:
                deathScript = ResolveDirectorScriptVA(kDirectorScriptDeathFallPavement);
                break;
            case 2:
                deathScript = ResolveDirectorScriptVA(kDirectorScriptDeathFallWater);
                break;
            case 4:
                deathScript = ResolveDirectorScriptVA(kDirectorScriptDeathFallGoo);
                break;
            default:
                deathScript = ResolveDirectorScriptVA(kDirectorScriptDeathGeneric);
                break;
        }
    }

    PushScriptReturnAddress(this, deathScript);
}

// PSX: WaitAnimationDone__8Director (DIRECTOR.CPP:4443, 0x8003EB14)
// Returns 1 when animation done, 0 when still waiting.
s32 Director::WaitAnimationDoneStep() {
    MARKFUNCTION(0x8003EB14);

    if (!scriptPtr) {
        return 0;
    }

    // PSX: FindNodeCRC(0x34=humanoidList, scriptPtr[1], 0)
    // then checks thing->model->animStructure->loopCount at +84.
    const u32 thingRef = static_cast<u32>(scriptPtr[1]);
    Humanoid* humanoid = FindHumanoidInHumanoidListByScriptRef(thingRef);
    if (humanoid && humanoid->model) {
        Model* model = static_cast<Model*>(humanoid->model);
        AnimStructure* anim = model ? static_cast<AnimStructure*>(model->animStructure) : nullptr;
        const s32 loopCount = anim ? anim->loopCount : 0;
        const s32 done = (loopCount != 0) ? 1 : 0;

        if (done) {
            scriptPtr += 2;
            return 1;
        }
        return 0;
    }

    return 0;
}

// PSX: ProcessDynamicAnimFunc__8Director (DIRECTOR.CPP:4469, 0x8003EB88)
void Director::ProcessDynamicAnimFunc() {
    MARKFUNCTION(0x8003EB88);

    if (!scriptPtr) {
        return;
    }

    field68 = 0;

    s32* cmd = scriptPtr;
    const DirectorOpcode opcode = static_cast<DirectorOpcode>(cmd[0]);

    if (opcode == DirectorOpcode::DynamicAnimWaitLoaded) {
        const u32 thingType = static_cast<u32>(cmd[1]);
        const s32 animEnum = cmd[2];
        scriptPtr = cmd + 3;

        const bool hasAnim =
            (g_characterManager && (g_characterManager->GetAnimation(thingType, animEnum) != nullptr));
        if (hasAnim) {
            field68 = 0;
        }
        else {
            // PSX retries this opcode until the animation is available.
            scriptPtr = cmd;
            field68 = 1;
        }
        return;
    }

    if (opcode == DirectorOpcode::DynamicAnimLoad) {
        const u32 thingType = static_cast<u32>(cmd[1]);
        const s32 animEnum = cmd[2];
        scriptPtr = cmd + 3;
        if (g_characterManager) {
            g_characterManager->LoadAnimationBatch(thingType, animEnum, nullptr);
        }
        return;
    }

    if (opcode == DirectorOpcode::DynamicAnimWaitCamera) {
        // PSX gates this on camera async animation data (camera+0x1D0),
        // not just camera object existence.
        Camera* camera = nullptr;
        if (g_display) {
            camera = g_display->GetCamera();
        }

        if (camera && camera->HasAsyncAnimLoaded()) {
            scriptPtr = cmd + 1;
            field68 = 0;
        }
        else {
            field68 = 1;
        }
        return;
    }

    if (opcode == DirectorOpcode::DynamicAnimUnload) {
        // PSX ProcessDynamicAnimFunc case 22 only calls UnloadAnimation(0, type, anim).
        // It does NOT touch humanoid flip state.
        const u32 thingType = static_cast<u32>(cmd[1]);
        const s32 animEnum = cmd[2];
        scriptPtr = cmd + 3;
        if (g_characterManager) {
            g_characterManager->UnloadAnimationBatch(thingType, animEnum);
        }
        return;
    }

    scriptPtr = cmd + 1;
    field68 = 0;
    // PSX: unknown dynamic animation tokens are ignored.
}

// PC-only. Call right after a loading screen finishes so that if a cutscene
// enables the widescreen bars immediately afterward, they appear already
// deployed instead of sliding/fading in on top of the fade the loading
// screen already did. Bars/alpha ramping back down (exit) still animate.
void Director::SkipNextWideScreenEnterAnim() {
    wsSkipEnterAnimUntil = Time::GetTimeInSeconds() + 3.0;
}

// PSX: HandleWideScreen__8Director (DIRECTOR.CPP:4539, 0x8003ECD4)
void Director::HandleWideScreen() {
    MARKFUNCTION(0x8003ECD4);

    const bool skipEnterAnim = (wsSkipEnterAnimUntil >= 0.0) &&
                               (Time::GetTimeInSeconds() <= wsSkipEnterAnimUntil);

    if (wsBarCurrent != wsBarTarget) {
        if (skipEnterAnim && wsBarTarget > wsBarCurrent) {
            wsBarCurrent = wsBarTarget;
            wsSkipEnterAnimUntil = -1.0;
        }
        else if (wsBarStep == 256) {
            wsBarCurrent = wsBarTarget;
        }
        else if (wsBarCurrent >= wsBarTarget) {
            wsBarCurrent -= wsBarStep;
            if (wsBarCurrent < wsBarTarget) {
                wsBarCurrent = wsBarTarget;
            }
        }
        else {
            wsBarCurrent += wsBarStep;
            if (wsBarCurrent > wsBarTarget) {
                wsBarCurrent = wsBarTarget;
            }
        }
    }

    if (wsBarCurrent && wsAlphaCurrent != wsAlphaTarget) {
        if (skipEnterAnim && wsAlphaTarget > wsAlphaCurrent) {
            wsAlphaCurrent = wsAlphaTarget;
            wsSkipEnterAnimUntil = -1.0;
        }
        else if (wsAlphaCurrent < wsAlphaTarget) {
            wsAlphaCurrent += wsAlphaStep;
            if (wsAlphaCurrent > wsAlphaTarget) {
                wsAlphaCurrent = wsAlphaTarget;
            }
        }
        else {
            wsAlphaCurrent -= wsAlphaStep;
            if (wsAlphaCurrent < wsAlphaTarget) {
                wsAlphaCurrent = wsAlphaTarget;
            }
        }
    }
}

// PSX: DrawWideScreenPolys__8Director (DIRECTOR.CPP:4582, 0x8003ED90)
// Draws two horizontal letterbox bars (top and bottom) and a blend mode triangle.
// PSX uses POLY_F4 primitives in the GPU ordering table at layer 3.
// PC: uses ScreenDraw to render equivalent overlay quads.
void Director::DrawWideScreenPolys() {
    MARKFUNCTION(0x8003ED90);

    if (wsBarCurrent == 0 || wsAlphaCurrent == 0) {
        return;
    }

    if (wsBarTarget >= 120) {
        const f32 barHeight = (static_cast<f32>(wsBarCurrent) / 240.0f) * SCREEN_HEIGHT;
        if (barHeight <= 0.0f) {
            return;
        }
        ScreenDraw::DrawColoredRect(0.0f, 0.0f, SCREEN_WIDTH, barHeight,
                                    0, 0, 0, (u8)wsAlphaCurrent);
        ScreenDraw::DrawColoredRect(0.0f, SCREEN_HEIGHT - barHeight, SCREEN_WIDTH, barHeight,
                                    0, 0, 0, (u8)wsAlphaCurrent);
        return;
    }

    u8 alpha = static_cast<u8>((wsAlphaCurrent < 255) ? wsAlphaCurrent : 255);

    // deploy = how far the bars have slid in, 0..1. In fade mode the bars are
    // full size and the alpha does the animation; in slide mode the bars grow
    // in from the edges and alpha stays solid.
    f32 deploy = 1.0f;
#if DIRECTOR_WIDESCREEN_SLIDE_BARS
    const f32 alphaTarget = (wsAlphaTarget > 0) ? static_cast<f32>(wsAlphaTarget) : 255.0f;
    deploy = Clamp(static_cast<f32>(wsAlphaCurrent) / alphaTarget, 0.0f, 1.0f);
    alpha = 255;
#endif

#if DIRECTOR_SMART_CUTSCENE_BORDERS
    constexpr f32 kAspectEpsilon = 0.01f;
    const f32 screenAspect = SCREEN_WIDTH / SCREEN_HEIGHT;

    if (screenAspect > TARGET_ASPECT_RATIO + kAspectEpsilon) {
        const f32 barWidth = (SCREEN_WIDTH - SCREEN_HEIGHT * TARGET_ASPECT_RATIO) * 0.5f * deploy;
        if (barWidth <= 0.0f) {
            return;
        }
        ScreenDraw::DrawColoredRect(0.0f, 0.0f, barWidth, SCREEN_HEIGHT,
                                    0, 0, 0, alpha);
        ScreenDraw::DrawColoredRect(SCREEN_WIDTH - barWidth, 0.0f, barWidth, SCREEN_HEIGHT,
                                    0, 0, 0, alpha);
        return;
    }

    if (screenAspect < TARGET_ASPECT_RATIO - kAspectEpsilon) {
        const f32 barHeight = (SCREEN_HEIGHT - SCREEN_WIDTH / TARGET_ASPECT_RATIO) * 0.5f * deploy;
        if (barHeight <= 0.0f) {
            return;
        }
        ScreenDraw::DrawColoredRect(0.0f, 0.0f, SCREEN_WIDTH, barHeight,
                                    0, 0, 0, alpha);
        ScreenDraw::DrawColoredRect(0.0f, SCREEN_HEIGHT - barHeight, SCREEN_WIDTH, barHeight,
                                    0, 0, 0, alpha);
        return;
    }

    // Exactly 16:9: the Vert- FOV already frames the shot, no bars needed.
    return;
#else
    const f32 barHeight = (static_cast<f32>(wsBarCurrent) / 240.0f) * deploy * SCREEN_HEIGHT;
    if (barHeight <= 0.0f) {
        return;
    }

    // Top bar
    ScreenDraw::DrawColoredRect(0.0f, 0.0f, SCREEN_WIDTH, barHeight,
                                0, 0, 0, alpha);
    // Bottom bar
    ScreenDraw::DrawColoredRect(0.0f, SCREEN_HEIGHT - barHeight, SCREEN_WIDTH, barHeight,
                                0, 0, 0, alpha);
#endif
}

// PSX: PurgeAnims__8Director (DIRECTOR.CPP:4662, 0x8003F0E4)
void Director::PurgeAnims() {
    MARKFUNCTION(0x8003F0E4);
    cleanUpTexAnim();
}

// PSX: DoesLevelHaveExtraMem__8Directorl (DIRECTOR.CPP:4669, 0x8003F104)
bool Director::DoesLevelHaveExtraMem(s32 level) {
    MARKFUNCTION(0x8003F104);
    return level >= 6 && (level < 9 || (level >= 11 && level < 15));
}

// PSX: updateVramAnims__8Director (DIRECTOR.CPP:2597, 0x8003BB90)
void Director::updateVramAnims() {
    MARKFUNCTION(0x8003BB90);

    bool updated = false;

    if (flipbookA) {
        reinterpret_cast<tFlipbook*>(flipbookA)->AdvanceFrame();
        updated = true;
    }
    if (flipbookB) {
        reinterpret_cast<tFlipbook*>(flipbookB)->AdvanceFrame();
        updated = true;
    }

    if (updated && g_game && g_game->GetWorld()) {
        g_game->GetWorld()->RefreshVRAMTexture();
    }
}

// PSX: cleanUpTexAnim__8Director (DIRECTOR.CPP:2611, 0x8003BBE0)
void Director::cleanUpTexAnim() {
    MARKFUNCTION(0x8003BBE0);

    if (flipbookA) {
        // Reset to frame 0 so the default face is shown when the NIS anim is removed.
        // On PSX, VRAM texture deletion restores the underlying slot; on PC we must
        // explicitly snap back to the first frame before removing the flipbook.
        auto* fb = reinterpret_cast<tFlipbook*>(flipbookA);
        fb->SetCycleCallback(nullptr);
        fb->SetFrame(0);
        fb->AdvanceFrame();
        delete fb;
        flipbookA = 0;
    }
    if (texAnimA) {
        auto* texAnim = reinterpret_cast<tRAMTexAnim*>(texAnimA);
        if (p3d::inventory) {
            for (s32 i = 0; i < texAnim->GetNumTextures(); ++i) {
                if (tTexture* texture = texAnim->GetTexture(i)) {
                    p3d::inventory->Remove(texture->GetName());
                }
            }
            p3d::inventory->Remove(texAnim->GetName());
        }
        texAnimA = 0;
    }

    if (flipbookB) {
        auto* fb = reinterpret_cast<tFlipbook*>(flipbookB);
        fb->SetCycleCallback(nullptr);
        fb->SetFrame(0);
        fb->AdvanceFrame();
        delete fb;
        flipbookB = 0;
    }
    if (texAnimB) {
        auto* texAnim = reinterpret_cast<tRAMTexAnim*>(texAnimB);
        if (p3d::inventory) {
            for (s32 i = 0; i < texAnim->GetNumTextures(); ++i) {
                if (tTexture* texture = texAnim->GetTexture(i)) {
                    p3d::inventory->Remove(texture->GetName());
                }
            }
            p3d::inventory->Remove(texAnim->GetName());
        }
        texAnimB = 0;
    }
}

// Script accessor methods - provide access to file-local script arrays
// for obstacle.cpp cutscene triggers (Door/Ladder).
s32* Director::GetNISDoor1Script() {
    return ResolveDirectorScriptVA(kDirectorScriptNisDoor1);
}

s32* Director::GetNISDoor1WithDialogScript() {
    return ResolveDirectorScriptVA(kDirectorScriptNisDoor1WithDialog);
}

s32* Director::GetNISLadder1Script() {
    return ResolveDirectorScriptVA(kDirectorScriptNisLadder1);
}

s32* Director::GetDeathScript() {
    return ResolveDirectorScriptVA(kDirectorScriptDeath);
}

s32* Director::GetLevelEndScript() {
    return ResolveDirectorScriptVA(kDirectorScriptLevelEnd);
}

s32* Director::GetGlobalScriptByIndex(s32 index) {
    RegisterKnownDirectorScriptRegions();

    if (index < 0 || index >= static_cast<s32>(sizeof(kGlobalScriptVirtual) / sizeof(kGlobalScriptVirtual[0]))) {
        return nullptr;
    }

    const u32 virtualAddress = kGlobalScriptVirtual[index];
    if (virtualAddress == 0) {
        return nullptr;
    }

    return ScriptPtrFromWord(static_cast<s32>(virtualAddress));
}

// PSX: runDirector (DIRECTOR.CPP:2570, 0x8003BB0C) - handler callback
void runDirector(Handler* h) {
    MARKFUNCTION(0x8003BB0C);
    if (g_director) {
        g_director->Process();
    }
}

// DrawDirectorOverlays (DIRECTOR.CPP:2578, 0x8003BB34) - handler callback
// We reorder and keep widescreen bars on top: DrawEffects(4096) -> DrawWideScreenPolys.
// HandleWideScreen no longer runs here - see the call site in Director::Process().
void DrawDirectorOverlays(Handler* h) {
    MARKFUNCTION(0x8003BB34);
    if (g_director) {
        // Overlay effects still sample PSX-style world VRAM pages. Ensure
        // overlay rendering starts from deterministic world texture state.
        if (p3d::context) {
            if (g_game && g_game->GetWorld()) {
                p3d::context->SetVRAMHandle(g_game->GetWorld()->GetVRAMHandle());
            }
            p3d::context->SetTexInfoOverride(false, 0);
            p3d::context->SetBlendMode(PDDI_BLEND_NONE);
            p3d::context->SetCullMode(PDDI_CULL_NONE);
        }

        Effects_DrawEffects(4096);

        if (p3d::context) {
            p3d::context->EnableZBuffer(false);
        }
        g_director->DrawWideScreenPolys();
        if (p3d::context) {
            p3d::context->EnableZBuffer(true);
        }
    }
}
