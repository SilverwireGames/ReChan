#include "gen/scoremgr.h"
#include "gen/game.h"
#include "gen/world.h"
#include "gen/handler.h"
#include "gen/ai.h"
#include "fe/hud.h"
#include "gen/time.h"
#include "gen/director.h"
#if NEW_CHEATS
#include "extra/cheats.h"
#endif

ScoreManager* g_scoreManager = nullptr;
static Handler* s_scoreMgrHandler = nullptr;

// PSX: CheckpointInfo::IsValid (SCOREMGR.CPP:910, 0x8004D72C)
bool CheckpointInfo::IsValid() const {
    MARKFUNCTION(0x8004D72C);

    if (!isValid) {
        return false;
    }

    if (!g_game || !g_game->GetWorld()) {
        return false;
    }

    const s32 curLevel = static_cast<s32>(g_game->GetWorld()->GetCurrentLevelIndex());
    const s32 curPetal = static_cast<s32>(g_game->GetWorld()->GetCurrentPetalIndex());

    if (levelIndex == curLevel && petalIndex == curPetal) {
        return true;
    }

    const_cast<CheckpointInfo*>(this)->SetValidState(0);
    return false;
}

// PSX: CheckpointInfo::SetValidState (SCOREMGR.CPP:935, 0x8004D798)
void CheckpointInfo::SetValidState(s32 state) {
    MARKFUNCTION(0x8004D798);

    if (state == 1) {
        isValid = 1;
        if (g_game && g_game->GetWorld()) {
            levelIndex = static_cast<s32>(g_game->GetWorld()->GetCurrentLevelIndex());
            petalIndex = static_cast<s32>(g_game->GetWorld()->GetCurrentPetalIndex());
        }
    }
    else {
        isValid = 0;
        levelIndex = -1;
        petalIndex = -1;
        field28 = -1;
    }
}

// PSX: scoreMgrPrivHandler__FP7Handler (SCOREMGR.CPP:233, 0x8004CC38)
void scoreMgrPrivHandler(Handler* h) {
    MARKFUNCTION(0x8004CC38);
    if (g_scoreManager) {
        g_scoreManager->Step();
    }
}

// PSX: __12ScoreManager (SCOREMGR.CPP:241, 0x8004CC5C)
ScoreManager::ScoreManager() {
    MARKFUNCTION(0x8004CC5C);
    drunkenMasterUnlocked = 0;
    g_scoreManager = this;
    HandleGameBegin();
}

// PSX: _._12ScoreManager (SCOREMGR.CPP:254, 0x8004CCA8)
ScoreManager::~ScoreManager() {
    MARKFUNCTION(0x8004CCA8);
    g_scoreManager = nullptr;
}

// PSX: InternalOpen__12ScoreManager (SCOREMGR.CPP:268, 0x8004CCD8)
void ScoreManager::InternalOpen() {
    MARKFUNCTION(0x8004CCD8);

    if (!s_scoreMgrHandler && g_game) {
        s_scoreMgrHandler = g_game->GetHandlerSet1().AddHandler(scoreMgrPrivHandler, 0x50);
    }

    InternalReset();
}

// PSX: InternalClose__12ScoreManager (SCOREMGR.CPP:284, 0x8004CD64)
void ScoreManager::InternalClose() {
    MARKFUNCTION(0x8004CD64);

    if (s_scoreMgrHandler) {
        s_scoreMgrHandler->RemoveFromList();
        delete s_scoreMgrHandler;
        s_scoreMgrHandler = nullptr;
    }
}

// PSX: InternalReset__12ScoreManager (SCOREMGR.CPP:294, 0x8004CD84)
void ScoreManager::InternalReset() {
    MARKFUNCTION(0x8004CD84);
}

// PSX: InitGameStats__12ScoreManager (SCOREMGR.CPP:303, 0x8004CD8C)
void ScoreManager::InitGameStats() {
    MARKFUNCTION(0x8004CD8C);

    for (s32 level = 0; level < 7; level++) {
        for (s32 petal = 0; petal < 3; petal++) {
            PetalStats& ps = petalStats[level * 3 + petal];
            ps.collectCount = 0;
            ps.goldDragons = 0;
            ps.fightScore = -2;
            ps.comboScore = 0;
            ps.styleScore = 0;
            ps.grade = 0;
        }
    }

    petalStats[0].fightScore = -1;
#if NEW_CHEATS
    ApplyProgressCheats();
#endif
}

// PSX: InitLevelStats__12ScoreManager (SCOREMGR.CPP:326, 0x8004CDEC)
void ScoreManager::InitLevelStats() {
    MARKFUNCTION(0x8004CDEC);

    currentFightScore = 0;
    currentComboScore = 0;
    currentStyleScore = 0;
    currentGrade = 0;
    currentCollectCount = 0;

    if (g_game && g_game->GetWorld()) {
        const u32 level = g_game->GetWorld()->GetCurrentLevelIndex();
        const u32 petal = g_game->GetWorld()->GetCurrentPetalIndex();
        if (level < 7 && petal < 3) {
            currentGoldDragons = petalStats[level * 3 + petal].goldDragons;
        }
        else {
            currentGoldDragons = 0;
        }
    }
    else {
        currentGoldDragons = 0;
    }

    checkpointFightScore = currentFightScore;
    checkpointComboScore = currentComboScore;
    checkpointStyleScore = currentStyleScore;
    checkpointGradeCollect = *reinterpret_cast<s32*>(&currentGrade);

    for (s32 i = 0; i < 10; i++) {
        collectibleRegistry[i] = 0;
        collectibleGot[i] = 0;
    }

    collectibleCount = 0;
    field484 = 0;
    fightingChainTotal = 0;
    fightingChainLast = 0;
    fightingChainTimer = 0;

    // PC
    secondsPassed = 0.0f;
}

// PSX: SetPar__12ScoreManager (SCOREMGR.CPP:364, 0x8004CEA0)
void ScoreManager::SetPar() {
    MARKFUNCTION(0x8004CEA0);

    // PSX: parValue = GetParPointValue__11WorldPoints(&WorldPointLists);
    parValue = WorldPoints_GetParValue();
    if (parValue <= 0)
        parValue = 1;
}

// PSX: OpenAllLevels__12ScoreManager (SCOREMGR.CPP:376, 0x8004CEE0)
void ScoreManager::OpenAllLevels() {
    MARKFUNCTION(0x8004CEE0);

    for (s32 level = 0; level < 7; level++) {
        for (s32 petal = 0; petal < 3; petal++) {
            petalStats[level * 3 + petal].fightScore = 0;
        }
    }
}

// PSX: GiveAllDragons__12ScoreManager (SCOREMGR.CPP:392, 0x8004CF24)
void ScoreManager::GiveAllDragons() {
    MARKFUNCTION(0x8004CF24);

    for (s32 level = 0; level < 5; level++) {
        for (s32 petal = 0; petal < 3; petal++) {
            PetalStats& ps = petalStats[level * 3 + petal];
            ps.collectCount = 10;
            ps.goldDragons = 10;
        }
    }

    // PSX sets specific entries for boss levels.
    // petalStats[5*3+1].grade = 1 => level 5 petal 1
    // petalStats[6*3+1].grade = 1 => level 6 petal 1
    // etc.
    petalStats[5 * 3 + 1].grade = 1;
    petalStats[6 * 3 + 1].grade = 1;
    petalStats[7 * 3 + 1].grade = 1;   // offset 138 => (138-28)/16 = 6.875? ...
    // PSX: a1[186]=1 => byte offset 186 from a1 start
    // That's petalStats byte offset (186-28)=158, 158/16=9.875 -> index 9, byte +14 = goldDragons
    // Actually the PSX sets raw byte offsets. Let me map precisely:
    // a1[74]=1  => byte 74  => petalStats offset 46 => entry 2 (idx=2), byte 14 => goldDragons
    // a1[106]=1 => byte 106 => petalStats offset 78 => entry 4 (idx=4), byte 14 => goldDragons
    // a1[138]=1 => byte 138 => petalStats offset 110 => entry 6 (idx=6), byte 14 => goldDragons
    // a1[186]=1 => byte 186 => petalStats offset 158 => entry 9 (idx=9), byte 14 => goldDragons
    // a1[266]=1 => byte 266 => petalStats offset 238 => entry 14 (idx=14), byte 14 => goldDragons
    // Wait - PSX uses _BYTE* indexing, so a1[74] is byte 74.
    // petalStats starts at byte 28. Entry size = 16 bytes.
    // byte 74 => offset 74-28=46 => entry 2 (46/16=2 r14) => goldDragons
    // byte 106 => offset 78 => entry 4 (78/16=4 r14) => goldDragons
    // byte 138 => offset 110 => entry 6 (110/16=6 r14) => goldDragons
    // byte 186 => offset 158 => entry 9 (158/16=9 r14) => goldDragons
    // byte 266 => offset 238 => entry 14 (238/16=14 r14) => goldDragons
    petalStats[2].goldDragons = 1;
    petalStats[4].goldDragons = 1;
    petalStats[6].goldDragons = 1;
    petalStats[9].goldDragons = 1;
    petalStats[14].goldDragons = 1;
}

// PSX: Step__12ScoreManager (SCOREMGR.CPP:418, 0x8004CF84)
void ScoreManager::Step() {
    MARKFUNCTION(0x8004CF84);
    StepFighting();

    // PC
    const bool directorInputLocked = g_director && g_director->scriptState != 0 && g_director->enableInput == 0;
    if (!g_directorActive && !directorInputLocked) {
#if HIGH_FPS_PLAY_PRESENTATION
        secondsPassed += 1.0f / 30.0f;
#else
        secondsPassed += g_time->GetDeltaTime();
#endif
    }
}

// PSX: HandleLevelBegin__12ScoreManager (SCOREMGR.CPP:429, 0x8004CFA4)
void ScoreManager::HandleLevelBegin() {
    MARKFUNCTION(0x8004CFA4);
    InitLevelStats();
}

// PSX: HandleLevelEnd__12ScoreManager (SCOREMGR.CPP:442, 0x8004CFC4)
void ScoreManager::HandleLevelEnd() {
    MARKFUNCTION(0x8004CFC4);

    currentGrade = CalcGrade();

    if (!g_game || !g_game->GetWorld()) {
        Print();
        return;
    }

    const u32 level = g_game->GetWorld()->GetCurrentLevelIndex();
    const u32 petal = g_game->GetWorld()->GetCurrentPetalIndex();

    if (level < 7 && petal < 3) {
        PetalStats& ps = petalStats[level * 3 + petal];

        if (ps.fightScore < currentFightScore) {
            ps.fightScore = currentFightScore;
        }
        if (ps.comboScore < currentComboScore) {
            ps.comboScore = currentComboScore;
        }
        if (ps.styleScore < currentStyleScore) {
            ps.styleScore = currentStyleScore;
        }
        if (ps.grade < currentGrade) {
            ps.grade = currentGrade;
        }
        if (ps.collectCount < currentCollectCount) {
            ps.collectCount = currentCollectCount;
        }
        if (ps.goldDragons < currentGoldDragons) {
            ps.goldDragons = currentGoldDragons;
        }
    }

    Print();
}

// PSX: HandleLevelAbort__12ScoreManager (SCOREMGR.CPP:478, 0x8004D0D4)
void ScoreManager::HandleLevelAbort() {
    MARKFUNCTION(0x8004D0D4);
}

// PSX: GetLevelEndRating__12ScoreManager (SCOREMGR.CPP:491, 0x8004D0DC)
s32 ScoreManager::GetLevelEndRating() {
    MARKFUNCTION(0x8004D0DC);

    const u8 grade = CalcGrade();

    switch (grade) {
        case 2:
            return 1;
        case 3:
        case 4:
            return 2;
        case 5:
            return 3;
        default:
            return 0;
    }
}

// PSX: OpenPetal__12ScoreManagerUlUl (SCOREMGR.CPP:527, 0x8004D144)
void ScoreManager::OpenPetal(u32 level, u32 petal) {
    MARKFUNCTION(0x8004D144);

    if (level >= 7 || petal >= 3) {
        return;
    }

    PetalStats& ps = petalStats[level * 3 + petal];
    if (ps.fightScore < 0) {
        ps.fightScore = -1;
    }
}

// PSX: HandleCheckpoint__12ScoreManager (SCOREMGR.CPP:542, 0x8004D184)
void ScoreManager::HandleCheckpoint() {
    MARKFUNCTION(0x8004D184);

    checkpointFightScore = currentFightScore;
    checkpointComboScore = currentComboScore;
    checkpointStyleScore = currentStyleScore;
    checkpointGradeCollect = *reinterpret_cast<s32*>(&currentGrade);

    for (s32 i = 0; i < 10; i++) {
        if (collectibleGot[i] == 1) {
            collectibleGot[i] = 2;
        }
    }
}

// PSX: HandleCheckpointBegin__12ScoreManager (SCOREMGR.CPP:562, 0x8004D1DC)
void ScoreManager::HandleCheckpointBegin() {
    MARKFUNCTION(0x8004D1DC);

    currentFightScore = checkpointFightScore;
    currentComboScore = checkpointComboScore;
    currentStyleScore = checkpointStyleScore;
    *reinterpret_cast<s32*>(&currentGrade) = checkpointGradeCollect;

    for (s32 i = 0; i < 10; i++) {
        collectibleRegistry[i] = 0;
        if (collectibleGot[i] != 2) {
            collectibleGot[i] = 0;
        }
    }
    collectibleCount = 0;

    fightingChainTotal = 0;
}

// PSX: Print__C12ScoreManager (SCOREMGR.CPP:584, 0x8004D234)
void ScoreManager::Print() const {
    MARKFUNCTION(0x8004D234);
}

// PSX: RegisterCollectible__12ScoreManagerPC11Collectiblei (SCOREMGR.CPP:611, 0x8004D260)
s32 ScoreManager::RegisterCollectible(const Collectible* collectible, s32 type) {
    MARKFUNCTION(0x8004D260);

    const s32 collectibleAddr = static_cast<s32>(reinterpret_cast<uintptr_t>(collectible));

    if (type == 1) {
        // The registry holds 10 entries; without this guard an 11th registration
        // (e.g. collectibles re-registered after a checkpoint respawn) writes past
        // the array into the fields that follow it, including collectibleCount.
        const s32 capacity = static_cast<s32>(sizeof(collectibleRegistry) / sizeof(collectibleRegistry[0]));
        if (collectibleCount >= 0 && collectibleCount < capacity) {
            collectibleRegistry[collectibleCount++] = collectibleAddr;
        }
        return 0;
    }

    if (type == 2) {
        const s32 index = static_cast<s32>(g_game->GetWorld()->GetCurrentLevelIndex()) * 3
            + static_cast<s32>(g_game->GetWorld()->GetCurrentPetalIndex());
        return petalStats[index].goldDragons;
    }

    return 0;
}

// PSX: RegisterGotCollectible__12ScoreManagerPC11Collectiblei (SCOREMGR.CPP:649, 0x8004D2E0)
s32 ScoreManager::RegisterGotCollectible(const Collectible* collectible, s32 type) {
    MARKFUNCTION(0x8004D2E0);

    const s32 collectibleAddr = static_cast<s32>(reinterpret_cast<uintptr_t>(collectible));

    if (type == 1) {
        for (s32 i = 0; i < 10; i++) {
            if (collectibleRegistry[i] == collectibleAddr) {
                collectibleGot[i] = 1;
                break;
            }
        }
        return ++currentCollectCount;
    }

    if (type == 2) {
        const s32 index = static_cast<s32>(g_game->GetWorld()->GetCurrentLevelIndex()) * 3
            + static_cast<s32>(g_game->GetWorld()->GetCurrentPetalIndex());
        const u8 goldDragons = ++currentGoldDragons;
        reinterpret_cast<u8*>(&checkpointGradeCollect)[2] = goldDragons;
        petalStats[index].goldDragons = goldDragons;
        return static_cast<s32>(reinterpret_cast<uintptr_t>(&petalStats[index]));
    }

    return 0;
}

// PSX: AddFightPoints__12ScoreManagerl (SCOREMGR.CPP:693, 0x8004D388)
void ScoreManager::AddFightPoints(s32 points) {
    MARKFUNCTION(0x8004D388);
    currentFightScore += points;
}

// PSX: AddComboPoints__12ScoreManagerl (SCOREMGR.CPP:698, 0x8004D39C)
void ScoreManager::AddComboPoints(s32 points) {
    MARKFUNCTION(0x8004D39C);
    currentComboScore += points;
}

// PSX: AddStylePoints__12ScoreManagerl (SCOREMGR.CPP:703, 0x8004D3B0)
void ScoreManager::AddStylePoints(s32 points) {
    MARKFUNCTION(0x8004D3B0);
    currentStyleScore += points;
}

// PSX: StepFighting__12ScoreManager (SCOREMGR.CPP:735, 0x8004D3C4)
void ScoreManager::StepFighting() {
    MARKFUNCTION(0x8004D3C4);

    if (fightingChainTimer > 0) {
        fightingChainTimer--;
        if (fightingChainTimer == 0) {
            BreakFightingChain();
        }
    }
}

// PSX: BreakFightingChain__12ScoreManager (SCOREMGR.CPP:762, 0x8004D408)
void ScoreManager::BreakFightingChain() {
    MARKFUNCTION(0x8004D408);

    if (fightingChainTotal != fightingChainLast) {
        AddComboPoints(fightingChainTotal);
    }

    fightingChainTotal = 0;
    fightingChainLast = 0;
    fightingChainTimer = 0;

    if (g_hud) {
        g_hud->TriggerBonusUpdate();
    }
}

// PSX: AddFightingPoints__12ScoreManagerl (SCOREMGR.CPP:780, 0x8004D45C)
void ScoreManager::AddFightingPoints(s32 points) {
    MARKFUNCTION(0x8004D45C);

    AddFightPoints(points);
    fightingChainLast = points;
    fightingChainTotal += points;
    fightingChainTimer += 15;

    if (g_hud) {
        g_hud->UpdateBonusScore();
    }
}

// PSX: HandleGameBegin__12ScoreManager (SCOREMGR.CPP:798, 0x8004D4C8)
void ScoreManager::HandleGameBegin() {
    MARKFUNCTION(0x8004D4C8);
#if NEW_CHEATS
    ResetCheats();
#endif
    InitGameStats();
    InitLevelStats();
}

// PSX: CalcGrade__12ScoreManager (SCOREMGR.CPP:809, 0x8004D518)
u8 ScoreManager::CalcGrade() {
    MARKFUNCTION(0x8004D518);

    if (parValue <= 0) {
        return 1;
    }

    const s32 pct = (100 * currentFightScore) / parValue;

    if (pct >= 95) {
        return 5;
    }
    if (pct >= 80) {
        return 4;
    }
    if (pct >= 65) {
        return 3;
    }
    if (pct >= 50) {
        return 2;
    }

    return 1;
}

// PSX: CalcGradeXTakes__12ScoreManagerUc (SCOREMGR.CPP:839, 0x8004D5AC)
s32 ScoreManager::CalcGradeXTakes(u8 grade) {
    MARKFUNCTION(0x8004D5AC);
    return (grade == 4 || grade == 5) ? 2 : 0;
}

// PSX: CalcGDrags__12ScoreManageri (SCOREMGR.CPP:845, 0x8004D5C0)
bool ScoreManager::CalcGDrags(s32 collectCount) {
    MARKFUNCTION(0x8004D5C0);
    return collectCount >= 10;
}

// PSX: GetTotalGoldDragon__12ScoreManager (SCOREMGR.CPP:851, 0x8004D5CC)
s32 ScoreManager::GetTotalGoldDragon() {
    MARKFUNCTION(0x8004D5CC);

    s32 total = 0;

    u32 curLevel = 0;
    u32 curPetal = 0;
    if (g_game && g_game->GetWorld()) {
        curLevel = g_game->GetWorld()->GetCurrentLevelIndex();
        curPetal = g_game->GetWorld()->GetCurrentPetalIndex();
    }

    for (s32 level = 0; level < 5; level++) {
        for (s32 petal = 0; petal < 3; petal++) {
            if (static_cast<u32>(level) == curLevel && static_cast<u32>(petal) == curPetal) {
                continue;
            }

            const PetalStats& ps = petalStats[level * 3 + petal];
            total += ps.goldDragons;
            if (CalcGDrags(ps.collectCount)) {
                total++;
            }
        }
    }

    total += currentGoldDragons;
    return total;
}

// PSX: IsDrunkenMasterSuitEnabled__12ScoreManager (SCOREMGR.CPP:883, 0x8004D69C)
bool ScoreManager::IsDrunkenMasterSuitEnabled() {
    MARKFUNCTION(0x8004D69C);

    const s32 totalDragons = GetTotalGoldDragon();

    if (!g_game || !g_game->GetWorld()) {
        return drunkenMasterUnlocked != 0;
    }

    const s32 levelID = g_game->GetWorld()->GetCurLevelID();

    bool enabled = false;
    if (levelID == 6) {
        enabled = totalDragons >= 6;
    }

    if (levelID == 7 && totalDragons >= 20) { // actually == 20 boh
        drunkenMasterUnlocked = 1;
    }

    return enabled || drunkenMasterUnlocked != 0;
}
