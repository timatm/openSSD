#include "debug.h"
#include "monitor/monitor.h"

#include "address_translation.h"
#include "memory_map.h"

bool nmcInterleaving;  // NMC Version interleaving: interleave blocks to different channel
uint32_t nmcPagesUsed; // How many pages have been written on each FC

static int32_t stashedBlocks[USER_DIES] = {[0 ...(USER_DIES - 1)] = -1};

void nmcEnableBlkInterleaving()
{
    ASSERT(!nmcInterleaving, "Already in NMC Block Interleaving Mode...");
    pr_info("Enable NMC Block Interleaving...");

    // TODO: flush data buffer and stash current block

    // reset
    nmcPagesUsed    = 0;
    nmcInterleaving = true;
}

void nmcDisableBlkInterleaving()
{
    ASSERT(nmcInterleaving, "Not in NMC Block Interleaving Mode...");
    pr_info("Disable NMC Block Interleaving...");
    pr_debug("NMC: nmcPagesUsed = %u", nmcPagesUsed);

    nmcPagesUsed    = 0;
    nmcInterleaving = false;

    // TODO: unstash current block
}

/**
 * @brief Stash the current working block of all the dies.
 */
void StashCurrentBlock()
{
    for (uint32_t iDie = 0; iDie < USER_DIES; ++iDie)
    {
        ASSERT(stashedBlocks[iDie] == -1, "Die[%u]: Cannot stash multiple blocks", iDie);
        stashedBlocks[iDie] = VDIE_ENTRY(iDie)->currentBlock;

#ifdef DEBUG
        // print free blocks and wait for user input
        VDIE_ENTRY(iDie)->currentBlock = SelectiveGetFromFbList(iDie, 1000, GET_FREE_BLOCK_NORMAL);
#else
        VDIE_ENTRY(iDie)->currentBlock = GetFromFbList(iDie, GET_FREE_BLOCK_NORMAL);
#endif
        if (VDIE_ENTRY(iDie)->currentBlock == BLOCK_FAIL)
        {
            pr_error("Die[%u]: Failed to allocate new block, restore stashed blocks", iDie);
            for (int iStashed = 0; iStashed <= iDie; ++iStashed)
            {
                VDIE_ENTRY(iDie)->currentBlock = stashedBlocks[iStashed];
                stashedBlocks[iStashed]        = -1;
            }
            break;
        }
        else
            pr_debug("Die[%u]: Replace working block %u -> %u", iDie, stashedBlocks[iDie],
                     VDIE_ENTRY(iDie)->currentBlock);
    }
}

/**
 * @brief Unstash the current working block of all the dies.
 */
void UnstashCurrentBlock()
{
    for (uint32_t iDie = 0; iDie < USER_DIES; ++iDie)
    {
        ASSERT(stashedBlocks[iDie] != -1, "Die[%u]: No Stashed Block", iDie);
        VDIE_ENTRY(iDie)->currentBlock = stashedBlocks[iDie];
        stashedBlocks[iDie]            = -1;
    }
}

uint32_t SelectiveGetFromFbList(uint32_t dieNo, uint32_t targetBlk, uint32_t mode)
{
    if (mode == GET_FREE_BLOCK_NORMAL)
    {
        if (VDIE_ENTRY(dieNo)->freeBlockCnt <= RESERVED_FREE_BLOCK_COUNT)
        {
            pr_error("Die[%u]: no available free block!!");
            return BLOCK_FAIL;
        }
    }
    else
    {
        pr_error("Die[%u]: Unexpected mode: %u!!", dieNo, mode);
        return BLOCK_FAIL;
    }

    // traverse fb list and find the target fb
    for (uint32_t vba = VDIE_ENTRY(dieNo)->headFreeBlock; vba != BLOCK_NONE; vba = VBLK_NEXT_IDX(dieNo, vba))
    {
        if (vba == targetBlk)
        {
            // update fb info
            VBLK_ENTRY(dieNo, vba)->free = 0;
            VDIE_ENTRY(dieNo)->freeBlockCnt--;

            // update neighbor blocks or head/tail
            if (VBLK_ENTRY(dieNo, vba)->prevBlock == BLOCK_NONE)
                VDIE_ENTRY(dieNo)->headFreeBlock = VBLK_NEXT_IDX(dieNo, vba); /* update HEAD */
            else
                VBLK_PREV_ENTRY(dieNo, vba)->nextBlock = VBLK_NEXT_IDX(dieNo, vba);

            if (VBLK_ENTRY(dieNo, vba)->nextBlock == BLOCK_NONE)
                VDIE_ENTRY(dieNo)->tailFreeBlock = VBLK_PREV_IDX(dieNo, vba); /* update TAIL */
            else
                VBLK_NEXT_ENTRY(dieNo, vba)->prevBlock = VBLK_PREV_IDX(dieNo, vba);

            // reset block link
            VBLK_ENTRY(dieNo, vba)->nextBlock = BLOCK_NONE;
            VBLK_ENTRY(dieNo, vba)->prevBlock = BLOCK_NONE;

            return vba;
        }
    }

    pr_error("Die[%u]: Target Blk[%u] not in the FB List!!", dieNo, targetBlk);
    return BLOCK_FAIL;
}
