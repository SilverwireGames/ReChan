// blockmgr.h = BlockManager reversed from PSX BLKMGR.CPP
#pragma once

#include "core.h"
#include "gen/manager.h"
#include "gen/block.h"
#include <vector>

// PSX uses 8 logical block-list slots. PC widens this for widescreen draw/load coverage.
static constexpr u32 kBlockManagerListCapacity = 16;

// BlockManager = manages level block loading, draw lists, demand loading
class BlockManager : public Manager {
public:
    BlockManager();
    ~BlockManager() override;

    // allocate block pool from WDB database
    void LoadBlocksFunc(const std::vector<DBVolume*>& volumes);

    // set up draw/load callback nodes
    void InternalOpen() override;

    void InternalClose() override;

    // load and parse block data from stream
    void LoadBlocks(u32 blockNum,
                    const u8* const* blkDataPtrs,
                    const u32* blkSizes,
                    u32 blkCount);

    // get block by index
    Block* GetBlock(u32 index);

    // resolve a block by its block number (used to walk the windowed
    // alreadyLoadedList instead of scanning the whole level per frame)
    Block* GetBlockByBlockNumber(u32 blockNum);

    // PSX: IsValidBlockNumber (BLKMGR.CPP:769, 0x80050C70)
    // Iterates loaded block list, returns true if blockNum is loaded.
    bool IsValidBlockNumber(u32 blockNum) const;

    // PSX: GetBlockNumber (BLKMGR.CPP:749, 0x80050C04)
    // Iterates loaded blocks, returns block->blockNum if pos is inside.
    u16 GetBlockNumber(const LVector& pos) const;

    // PC: searches all blocks (including unloaded) by bounding box.
    u16 GetBlockNumberAllBlocks(const LVector& pos) const;

    // PSX: InActiveList (BLKMGR.CPP:825, 0x80050D44)
    // Checks activeList for blockNum.
    bool InActiveList(u32 blockNum) const;

    // PC: drawn in widescreen but outside the PSX active range.
    bool IsVisualOnlyBlock(u32 blockNum) const;

    // PSX: InDrawList (BLKMGR.CPP:807, 0x80050CF4)
    // Checks drawList for blockNum.
    bool InDrawList(u32 blockNum) const;

    // PSX: InLoadList (BLKMGR.CPP:785, 0x80050CB4)
    // Checks toBeLoadedList for blockNum.
    bool InLoadList(u32 blockNum) const;

    // PSX: CrossedBoundary (BLKMGR.CPP:486, 0x800506BC)
    bool CrossedBoundary() const;

    // PSX: DemandLoading (BLKMGR.CPP:519, 0x800506E8)
    s32 DemandLoading();

    // PSX: UpdateToBeLoadedList (BLKMGR.CPP:852, 0x80050D94)
    bool UpdateToBeLoadedList(u32 blockNum);

    // PSX: UpdateAlreadyLoadedList (updates alreadyLoadedList from loaded blocks)
    void UpdateAlreadyLoadedList();

    u32 GetNumBlocks() const { return totalBlocks; }

    // windowed set of currently-resident blocks
    u32 GetAlreadyLoadedCount() const { return alreadyLoadedCount; }
    u16 GetAlreadyLoadedBlockNum(u32 index) const { return alreadyLoadedList[index]; }

    // PSX player death-volume handling clears BlockManager +0x8C before queuing death.
    void SetDeathVolumeFlag(s32 value) { flag3 = (u32)value; }

    // Block array access (for rendering iteration)
    Block* GetBlocks() { return blocks.data(); }
    const Block* GetBlocks() const { return blocks.data(); }

private:
    // +32: numBlocks (max blocks that can be loaded at once)
    u32 numBlocks;
    // +40: Block array (pointer on PSX, vector on PC)
    std::vector<Block> blocks;
    // +48: totalBlocks (total blocks in level)
    u32 totalBlocks;
    // +52: currentBlockNum
    u32 currentBlockNum;

    // PSX +64: toBeLoadedList - block numbers queued for loading
    u16 toBeLoadedList[kBlockManagerListCapacity];
    u32 toBeLoadedCount;

    // PSX +92: drawListCount + drawList - blocks in draw range
    u32 drawListCount;
    u16 drawList[kBlockManagerListCapacity];

    // PSX +78: alreadyLoadedList - blocks resident in memory
    u32 alreadyLoadedCount;
    u16 alreadyLoadedList[kBlockManagerListCapacity];

    // PSX +112: activeCount + activeList - blocks whose things are activated
    u32 activeCount;
    u16 activeList[kBlockManagerListCapacity];

    // +132-140: flags (all initialized to 1)
    u32 flag1;
    u32 flag2;
    u32 flag3;

    // +160: loadingState (0x1000 = complete)
    u32 loadingState;

    // +164: lastAddedBlockNum (set when a block is added to loaded list)
    u32 lastAddedBlockNum;
};

// PSX: gp scope, defined in world.cpp
extern BlockManager* g_blockManager;
