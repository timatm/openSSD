#include "debug.h"
#include "stdbool.h"
#include "string.h" /* for strncpy */
#include "stddef.h" /* for offsetof */

#include "memory_map.h"
#include "nmc_mapping.h"
#include "address_translation.h"

extern void EvictDataBufEntry(unsigned int originReqSlotTag);

/* -------------------------------------------------------------------------- */
/*                        internal data for NMC mapping                       */
/* -------------------------------------------------------------------------- */

static NMC_MAPPING_TABLE *nmcMappingTablePtr, *nmcMappingTableBufPtr;
#define NMC_CH_MAP(iCh)              (&nmcMappingTablePtr->nmcChMappingTable[(iCh)])
#define NMC_CH_MAP_LIST(iCh, iEntry) (NMC_CH_MAP((iCh))->pba[(iEntry)])
#define NMC_CH_MAP_PRE_INFO(iCh)     (&NMC_CH_MAP(iCh)->pre_info)
#define NMC_CH_MAP_POST_INFO(iCh)    (&NMC_CH_MAP(iCh)->post_info)

#define NMC_CH_MAP_BUFFER(iCh)      (nmcMappingTableBufPtr->nmcChMappingTable[(iCh)])
#define NMC_CH_MAP_BUFFER_ADDR(iCh) ((uintptr_t)NMC_CH_MAP_BUFFER((iCh)).byte)

static bool nmcRecordMapping;                           // record the newly allocated block or not
static NMC_MAPPING_LOC nmcNewMappingLoc[USER_CHANNELS]; // target loc for mapping tables of new file

// get the PBA of the working mapping table on specific flash channel
#define NMC_NEW_MAPPING_PBLK_ON(iCh) (NMC_MAPPING_DIR2PBLK(nmcNewMappingLoc[(iCh)].iDir))

// recover the blocks used by NMC subsystem during start up
static NMC_BLOCK_TABLE *nmcBlockTablePtr, *nmcTmpBlockTablePtr;

#define NMC_BLOCK_TABLE_RESET(pTbl)            (memset((pTbl), 0, sizeof(NMC_BLOCK_TABLE)))
#define NMC_BLOCK_TABLE_AT(pTbl, iDie, iBlk)   (&(pTbl)->block[(iDie)][(iBlk)])
#define NMC_USED_BLOCK(iDie, iBlk)             (NMC_BLOCK_TABLE_AT(nmcBlockTablePtr, (iDie), (iBlk)))
#define NMC_USED_BLOCK_CW(iCh, iWay, iBlk)     (NMC_USED_BLOCK(PCH2VDIE((iCh), (iWay)), (iBlk)))
#define NMC_TMP_USED_BLOCK(iDie, iBlk)         (NMC_BLOCK_TABLE_AT(nmcTmpBlockTablePtr, (iDie), (iBlk)))
#define NMC_TMP_USED_BLOCK_CW(iCh, iWay, iBlk) (NMC_TMP_USED_BLOCK(PCH2VDIE((iCh), (iWay)), (iBlk)))

typedef struct
{
    uint16_t iCh, iWay, iDir;
} NMC_SKIPPED_DIR_BLKS;

const NMC_SKIPPED_DIR_BLKS skipList[] = {
    {.iCh = 4, .iWay = 0, .iDir = 0},
};
const size_t numSkipDirBlks = sizeof(skipList) / sizeof(NMC_SKIPPED_DIR_BLKS);

static bool nmcSkippedDirBlk(uint8_t iCh, uint16_t iWay, uint16_t iDir);
static void nmcUpdateNewMappingLoc(uint8_t iCh);
static bool nmcFreePageCheck(const NMC_CH_MAPPING_TABLE *page);
static bool nmcRecordFileBlks(uint8_t iCh, const NMC_CH_MAPPING_TABLE *file);
static void nmcClearMapping();
static void nmcFlushMapping(uint32_t iCh);
static void nmcDumpCachedMappingTable();
static void nmcDumpNewMappingLoc(uint8_t iCh);

/* -------------------------------------------------------------------------- */
/*                      public interface implementations                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize the NMC mapping related data.
 *
 * Since the NMC Mapping uses the reserved area of DRAM, we must first check that the
 * predefined area does not exceed the size of DRAM.
 *
 * Once we finish the address check, we need to reset the NMC Mapping related data.
 */
void nmcInitMapping()
{
    // check mapping table size
    STATIC_ASSERT(offsetof(NMC_CH_MAPPING_TABLE, pre_info) == NMC_CH_MAPPING_PREFIX_INFO_OFFSET);
    STATIC_ASSERT(offsetof(NMC_CH_MAPPING_TABLE, pba) == NMC_CH_MAPPING_TABLE_OFFSET);
    STATIC_ASSERT(offsetof(NMC_CH_MAPPING_TABLE, post_info) == NMC_CH_MAPPING_POSTFIX_INFO_OFFSET);
    STATIC_ASSERT(sizeof(NMC_CH_MAPPING_TABLE) == BYTES_PER_SLICE);

    STATIC_ASSERT(NMC_DATA_BUFFER_END_ADDR < RESERVED1_END_ADDR);
    STATIC_ASSERT(RESERVED1_START_ADDR <= NMC_START_ADDR);
    STATIC_ASSERT(NMC_END_ADDR <= RESERVED1_END_ADDR);

    pr_info("NMC: Initializing NMC Mapping...");
    nmcInitFileTable();

    // reset mapping data
    nmcRecordMapping      = false;
    nmcMappingTablePtr    = (NMC_MAPPING_TABLE *)NMC_MAPPING_TABLE_ADDR;
    nmcMappingTableBufPtr = (NMC_MAPPING_TABLE *)NMC_MAPPING_TABLE_BUFFER_ADDR;
    nmcBlockTablePtr      = (NMC_BLOCK_TABLE *)NMC_BLOCK_TABLE_ADDR;
    nmcTmpBlockTablePtr   = (NMC_BLOCK_TABLE *)NMC_TMP_BLOCK_TABLE_ADDR;

    // dump the mapping table address of each die for debugging
    for (uint8_t iCh = 0; iCh < USER_CHANNELS; ++iCh)
        pr_debug("Ch[%u]: NMC PBA Mapping at 0x%p", iCh, NMC_CH_MAP(iCh));

    NMC_BLOCK_TABLE_RESET(nmcBlockTablePtr);

    // recovery use the mapping table as buffer
    nmcRecoverMappingDir();
}

/**
 * @brief Create a mapping table for the new file.
 *
 * To identify the mapping tables, we use the filename as the table title.
 *
 * To check whether the SSD have enough spaces to store the file, the application should
 * inform the fw how many blocks the file needed.
 *
 * @note The required number of blocks will be treated as a limit; once the number of used
 * blocks exceeds the number of blocks required here, the write requests will be blocked.
 *
 * @warning The filename should not be too long.
 *
 * @param filename The name of the file for NMC application.
 * @param filetype The type of the file for NMC application.
 * @param nblks The number of blocks required by each die to store the new file.
 * @return int The status code (`SC_VENDOR_NMC_*`), 0 for no error.
 */
int nmcNewMapping(const char *filename, uint32_t filetype, uint32_t nblks)
{
    // can only serve one file at a time
    if (nmcRecordMapping)
    {
        pr_error("NMC: the previous table must be freed before creating a new one!!");
        nmcDumpCachedMappingTable();
        return SC_VENDOR_NMC_MAPPING_REOPENED;
    }

    // limited length of filename
    if (strlen(filename) > NMC_FILENAME_MAX_BYTES)
    {
        pr_error("NMC filename too long!!");
        return SC_VENDOR_NMC_MAPPING_FILENAME_TOO_LONG;
    }
    pr_info("NMC: Alloc %u blks/ch for file \"%s\" (type: %u)", nblks, filename, filetype);

    // TODO: NMC: check capacity

    // assign new mapping table title
    nmcRecordMapping = true;
    for (uint32_t iCh = 0; iCh < USER_CHANNELS; ++iCh)
    {
        // check file type first
        switch (filetype)
        {
        case NMC_FILE_TYPE_MODEL_UNET:
            NMC_CH_MAP_POST_INFO(iCh)->type = NMC_FILE_TYPE_MODEL_UNET;
            break;

        case NMC_FILE_TYPE_IMAGE_TIFF:
            NMC_CH_MAP_POST_INFO(iCh)->type = NMC_FILE_TYPE_IMAGE_TIFF;
            break;

        default:
            ASSERT(0, "Unsupported file \"%s\" (%u) for NMC mapping...", filename, filetype);
            break;
        }

        NMC_CH_MAP_POST_INFO(iCh)->magic = NMC_MAPPING_DIR_BLK_USED;
        NMC_CH_MAP_PRE_INFO(iCh)->count  = 0;
        NMC_CH_MAP_POST_INFO(iCh)->limit = nblks;
        strncpy(NMC_CH_MAP_POST_INFO(iCh)->title, filename, NMC_FILENAME_MAX_BYTES);
    }

    nmcEnableBlkInterleaving();

    // assert all the current blocks are empty
    VIRTUAL_BLOCK_ENTRY *currentBlk;
    for (uint32_t iDie = 0; iDie < USER_DIES; ++iDie)
    {
        currentBlk = VBLK_ENTRY(iDie, VDIE_ENTRY(iDie)->currentBlock);
        ASSERT(!currentBlk->currentPage, "Die[%u]: Current block is not Empty...");
    }

    // Add the current blocks as first blocks
    for (uint32_t iCh = 0, iDie, iPBlk; iCh < USER_CHANNELS; ++iCh)
    {
        iDie  = PCH2VDIE(iCh, 0);
        iPBlk = PBLK_ENTRY(iDie, VDIE_ENTRY(iDie)->currentBlock)->remappedPhyBlock;

        NMC_CH_MAP_LIST(iCh, 0).blkNo   = iPBlk;
        NMC_CH_MAP_LIST(iCh, 0).wayNo   = 0;
        NMC_CH_MAP_PRE_INFO(iCh)->count = 1;
    }

    return SC_VENDOR_NMC_SUCCESS;
}

/**
 * @brief Release the mapping table created by last application.
 *
 * @return int The status code (`SC_VENDOR_NMC_*`), 0 for no error.
 */
int nmcFreeMapping(uint32_t cmdSlotTag, bool force)
{
    if (!nmcRecordMapping && !force)
    {
        pr_error("NMC: Mapping table had already been disabled!!");
        nmcDumpCachedMappingTable();
        return SC_VENDOR_NMC_MAPPING_DISABLED;
    }

    pr_info("NMC: Free the mapping table on each flash channel");

    // flush cached data
    FlushDataBuf(cmdSlotTag);

    // flush mapping tables
    for (uint8_t iCh = 0; iCh < USER_CHANNELS; ++iCh)
        if (NMC_CH_MAP_PRE_INFO(iCh)->count)
        {
            pr_info("Ch[%u]: Start Flushing Cached Mapping Table...", iCh);
            nmcFlushMapping(iCh);
        }

    // reset NMC mapping related data
    nmcRecordMapping = false;

    // reset mapping table
    nmcClearMapping();
    nmcDisableBlkInterleaving();

    return SC_VENDOR_NMC_SUCCESS;
}

/**
 * @brief Record the PBA of the newly allocated block if needed.
 *
 * If a new block is chosen to serve a request and the fw is in NMC mode, we should record
 * the physical address that block.
 *
 * After the PBA of the given block is recorded, we may need to flush the mapping table if
 * the mapping table is full.
 *
 * @param iDie The source die of the newly allocated block.
 * @param vba The virtual slice address of the newly allocated block.
 */
void nmcRecordBlock(uint32_t iDie, uint32_t vba)
{
    uint16_t iCh, iWay, pba;

    if (!nmcRecordMapping)
    {
        pr_debug("Die[%u]: Not in NMC mode, skip recording VBA[%u]", iDie, vba);
        return;
    }

    iCh  = Vdie2PchTranslation(iDie);
    iWay = Vdie2PwayTranslation(iDie);

    if (NMC_CH_MAP_PRE_INFO(iCh)->count > NMC_CH_MAP_POST_INFO(iCh)->limit)
    {
        pr_debug("Used: %u", NMC_CH_MAP_PRE_INFO(iCh)->count);
        pr_debug("Required: %u", NMC_CH_MAP_POST_INFO(iCh)->limit);
        ASSERT(0, "C/W[%u/%u]: Failed to record newly allocated VBA[%u]", iCh, iWay, vba);
    }

    // get physical address info
    pba = PBLK_ENTRY(iDie, VBA2PBA_TBS(vba))->remappedPhyBlock;
    pr_info("Die[%u]: The newly allocated block is VBA[%u] (PBA[%u])", iDie, vba, pba);

    // push pba to the pba list on its channel
    NMC_CH_MAP_LIST(iCh, NMC_CH_MAP_PRE_INFO(iCh)->count).blkNo = pba;
    NMC_CH_MAP_LIST(iCh, NMC_CH_MAP_PRE_INFO(iCh)->count).wayNo = iWay;
    NMC_CH_MAP_PRE_INFO(iCh)->count++;

    // currently only support 1 mapping table per file
    if (NMC_CH_MAP_PRE_INFO(iCh)->count == NMC_CH_MAPPING_LIST_ENTRIES)
        ASSERT(0, "NMC: Use too much blocks by '%s'!", NMC_CH_MAP_POST_INFO(iCh)->title);
}

/**
 * @brief Recover the NMC location table and select a location for next file.
 *
 * During initialization stage, we need to scan the directory blocks for rebuilding the
 * location table for the future NMC applications.
 *
 * Also, since we have to prevent the firmware from writing the mapping table to the used
 * pages, we also need to find the first unused page in the directory blocks. And to find
 * the unused page, we can sequentially scan the directory blocks since the fw persist the
 * mapping tables sequentially.
 *
 * Because both the two things need to scan the directory blocks, we handle the both tasks
 * in this function.
 *
 * @note Since the data is distributed to all channels evenly, the mapping table for all
 * channels should be placed in the same page.
 */
void nmcRecoverMappingDir()
{
    uint32_t iReqEntry;
    NMC_MAPPING_LOC cursor;

    pr_info("NMC: Scanning Directory Blocks for Recovering Location Table...");
    for (uint8_t iCh = 0, done; iCh < USER_CHANNELS; ++iCh)
    {
        done = 0;
        for (cursor.iWay = 0; !done && cursor.iWay < USER_WAYS; ++cursor.iWay)
        {
            // search the predefined dir blocks
            for (cursor.iDir = 0; !done && cursor.iDir < NMC_MAPPING_DIR_PBLK_PER_DIE; ++cursor.iDir)
            {
                // skip specific dir blocks
                if (nmcSkippedDirBlk(iCh, cursor.iWay, cursor.iDir))
                    continue;

                pr_debug("NMC: C/W[%u/%u]: Check Dir[%u]", iCh, cursor.iWay, cursor.iDir);
                for (cursor.iPage = 0; !done && cursor.iPage < USER_PAGES_PER_BLOCK; ++cursor.iPage)
                {
                    // prepare request and data buffer for reading pages
                    iReqEntry = GetFromFreeReqQ();

                    // data buffer not initialized yet, don't use AllocateDataBuf()
                    REQ_ENTRY(iReqEntry)->reqType                       = REQ_TYPE_NAND;
                    REQ_ENTRY(iReqEntry)->reqCode                       = REQ_CODE_READ;
                    REQ_ENTRY(iReqEntry)->reqOpt.dataBufFormat          = REQ_OPT_DATA_BUF_ADDR;
                    REQ_ENTRY(iReqEntry)->reqOpt.nandAddr               = REQ_OPT_NAND_ADDR_PHY_ORG;
                    REQ_ENTRY(iReqEntry)->reqOpt.nandEcc                = REQ_OPT_NAND_ECC_ON;
                    REQ_ENTRY(iReqEntry)->reqOpt.nandEccWarning         = REQ_OPT_NAND_ECC_WARNING_ON;
                    REQ_ENTRY(iReqEntry)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
                    REQ_ENTRY(iReqEntry)->reqOpt.blockSpace             = REQ_OPT_BLOCK_SPACE_TOTAL;
                    REQ_ENTRY(iReqEntry)->logicalSliceAddr              = REQ_OPT_BLOCK_SPACE_TOTAL;
                    REQ_ENTRY(iReqEntry)->dataBufInfo.addr              = NMC_CH_MAP_BUFFER_ADDR(iCh);
                    REQ_ENTRY(iReqEntry)->nandInfo.physicalCh           = iCh;
                    REQ_ENTRY(iReqEntry)->nandInfo.physicalWay          = cursor.iWay;
                    REQ_ENTRY(iReqEntry)->nandInfo.physicalBlock        = NMC_MAPPING_DIR2PBLK(cursor.iDir);
                    REQ_ENTRY(iReqEntry)->nandInfo.physicalPage         = cursor.iPage;

                    // issue request
                    pr_debug("NMC: Reading Blk[%u].Page[%u]...", NMC_MAPPING_DIR2PBLK(cursor.iDir), cursor.iPage);
                    SelectLowLevelReqQ(iReqEntry);
                    SyncAllLowLevelReqDone();

                    // check page content
                    memcpy(NMC_CH_MAP(iCh), (void *)NMC_CH_MAP_BUFFER_ADDR(iCh), BYTES_PER_DATA_REGION_OF_SLICE);

                    if (NMC_CH_MAP_POST_INFO(iCh)->magic == NMC_MAPPING_DIR_BLK_USED)
                    {
                        // record the blocks used
                        nmcRecordFileBlks(iCh, NMC_CH_MAP(iCh));

                        // record mapping table location
                        nmcFileTableUpdate(iCh, cursor, NMC_CH_MAP_POST_INFO(iCh)->title,
                                           NMC_CH_MAP_POST_INFO(iCh)->type);
                    }
                    else if (NMC_CH_MAP_POST_INFO(iCh)->magic == NMC_MAPPING_DIR_BLK_UNUSED)
                    {
                        // deep check
                        pr_debug("NMC: a page with UNUSED mark found, do deep check...");
                        done = nmcFreePageCheck(NMC_CH_MAP(iCh));

                        if (done)
                        {
                            // if first empty page found, mark it next dir block
                            nmcNewMappingLoc[iCh] = cursor;

                            // the other pages should be all empty, so we can goto next channel
                            pr_info("NMC: unused page found, stop scanning on Ch[%u]\n\n", iCh);
                        }
                        else
                        {
                            pr_warn("Fake unused page, details:");
                            nmcDumpMappingTable((void *)NMC_CH_MAP(iCh), false);
                        }
                    }
                    else
                        pr_warn("Unexpected magic (%x)!", NMC_CH_MAP_POST_INFO(iCh)->magic);
                }
            }
        }
        ASSERT(done, "Ch[%u]: No empty page found in directory blocks.", iCh);
    }

    // dump the free pages on each flash channel for the new mapping tables
    for (uint8_t iCh = 0; iCh < USER_CHANNELS; ++iCh)
        nmcDumpNewMappingLoc(iCh);
}

bool nmcPhyBlockUsed(uint32_t iDie, uint32_t iBlk) { return NMC_USED_BLOCK(iDie, iBlk)->isUsed; }

/* -------------------------------------------------------------------------- */
/*                      internal utility implementations                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Skip the given directory block or not.
 *
 * The NMC subsystem allow user to reserve some physical blocks for storing the mapping
 * tables by modifying the variable `NMC_MAPPING_DIR_PBLKS`.
 *
 * However, since this variable can only configure same block offsets for all the dies,
 * instead of configuring each die separately, the NMC subsystem needs to check whether
 * a specific block should be skipped during the two functions `nmcRecoverMappingDir()`
 * and `nmcUpdateNewMappingLoc()`, to overcome the bad block problems.
 *
 * @sa `NMC_MAPPING_DIR_PBLKS`, `nmcRecoverMappingDir()`, `nmcUpdateNewMappingLoc()`.
 *
 * @param iCh The physical channel number of the target directory block.
 * @param iWay The physical way number of the target directory block.
 * @param iDir The directory number of the target directory block.
 * @return true Skip the specified block.
 * @return false Do not skip the specified block.
 */
static bool nmcSkippedDirBlk(uint8_t iCh, uint16_t iWay, uint16_t iDir)
{
    for (size_t i = 0; i < numSkipDirBlks; ++i)
        if (iCh == skipList[i].iCh && iWay == skipList[i].iWay && iDir == skipList[i].iDir)
        {
            pr_info("Skip dirBlk C/W/D=%u/%u/%u", iCh, iWay, iDir);
            return true;
        }
    return false;
}

static void nmcUpdateNewMappingLoc(uint8_t iCh)
{
    nmcNewMappingLoc[iCh].iPage++;

    if (nmcNewMappingLoc[iCh].iPage == USER_PAGES_PER_BLOCK)
    {
        do
        {
            nmcNewMappingLoc[iCh].iWay++;
            nmcNewMappingLoc[iCh].iPage = 0;
            pr_debug("Ch[%u]: Update Mapping Table Loc.iWay to %u", iCh, nmcNewMappingLoc[iCh].iWay);

            if (nmcNewMappingLoc[iCh].iWay == USER_WAYS)
            {
                // reach last directory block
                if (nmcNewMappingLoc[iCh].iDir == (NMC_MAPPING_DIR_PBLK_PER_DIE - 1))
                    ASSERT(0, "Ch[%u]: No more blocks can be used to store mapping table", iCh);

                nmcNewMappingLoc[iCh].iDir++;
                nmcNewMappingLoc[iCh].iWay = 0;
                pr_debug("Ch[%u]: Update Mapping Table Loc.iDir to %u", iCh, nmcNewMappingLoc[iCh].iDir);
            }
        } while (nmcSkippedDirBlk(iCh, nmcNewMappingLoc[iCh].iWay, nmcNewMappingLoc[iCh].iDir));
    }

    // dump info after updating
    nmcDumpNewMappingLoc(iCh);
}

/**
 * @brief Whether the target flash page is a free page (unused page).
 *
 * If a flash page is free, all the bytes of its content should be 0xFF.
 *
 * @sa https://stackoverflow.com/a/28563801/21895256 for reference
 *
 * @param page The target flash page.
 * @return true This page is free (all bytes 0xFF).
 * @return false Otherwise.
 */
static bool nmcFreePageCheck(const NMC_CH_MAPPING_TABLE *page)
{
    const uint8_t *ptr = (uint8_t *)page;
    for (uint32_t iByte = 0; iByte < BYTES_PER_DATA_REGION_OF_SLICE; ++iByte)
        if (ptr[iByte] != 0xFF)
            return false;
    return true;
}

static bool nmcRecordFileBlks(uint8_t iCh, const NMC_CH_MAPPING_TABLE *file)
{
    // dump basic info of this file
    nmcDumpMappingTable((void *)file, true);

    // skip recording file with unreasonable number of blocks
    if (file->pre_info.count > NMC_MAX_BLKS_PER_FC)
    {
        pr_warn("Ch[%u]: Too many blocks (%u) used", iCh, file->pre_info.count);
        return false;
    }

    // record all blocks used
    uint16_t nNewBlks = 0;
    for (size_t i = 0, iBlk, iWay; i < file->pre_info.count; ++i)
    {
        iWay = NMC_CH_MAP_LIST(iCh, i).wayNo;
        iBlk = NMC_CH_MAP_LIST(iCh, i).blkNo;

        if (iWay < USER_WAYS && iBlk < NMC_MAX_BLKS_PER_FC)
        {
            // view as new blk if not marked as used
            nNewBlks += !NMC_USED_BLOCK_CW(iCh, iWay, iBlk)->isUsed;

            NMC_USED_BLOCK_CW(iCh, iWay, iBlk)->isUsed = true;
            pr_debug("Mark W[%u].B[%u] as Used", iWay, iBlk);
        }
        else
            pr_warn("W[%u].B[%u] is Out-of-range !!", iWay, iBlk);
    }

    pr_info("NMC: %u blks (%u new) recorded", iCh, file->pre_info.count, nNewBlks);
    return true;
}

static void nmcClearMapping()
{
    pr_info("NMC: Clearing Cached Mapping Table...");
    for (uint32_t iCh = 0; iCh < USER_CHANNELS; ++iCh)
    {
        NMC_CH_MAP_POST_INFO(iCh)->magic = 0;
        NMC_CH_MAP_PRE_INFO(iCh)->count  = 0;
        NMC_CH_MAP_POST_INFO(iCh)->limit = 0;
        NMC_CH_MAP_POST_INFO(iCh)->type  = NMC_FILE_TYPE_NONE;
        memset(NMC_CH_MAP_POST_INFO(iCh)->title, 0, NMC_FILENAME_MAX_BYTES);

        for (uint32_t iBlk = 0; iBlk < NMC_CH_MAPPING_LIST_ENTRIES; ++iBlk)
            NMC_CH_MAP_LIST(iCh, iBlk).addr = BLOCK_NONE;
    }
}

/**
 * @brief Persist the mapping table to the underlying flash memory.
 *
 * When the mapping table is full or the all the data of the file have beed written, we
 * need to flush the mapping table to the flash memory.
 *
 * During initialization, we should already have obtained the directory block info for
 * persisting the next mapping table, so here we can determine the target page based on
 * this info.
 *
 * @param iDie The source die of the mapping table to be persisted.
 */
static void nmcFlushMapping(uint32_t iCh)
{
    uint32_t iReqEntry;
    uintptr_t dataBufAddr;

    const char *title;
    NMC_FILE_TYPES type;

    // reuse the mapping table as data+spare region buffer
    dataBufAddr = NMC_CH_MAP_BUFFER_ADDR(iCh);
    memcpy((void *)dataBufAddr, NMC_CH_MAP(iCh), BYTES_PER_SLICE);

    pr_info("Ch[%u] Dump mapping table (data buffer) before flushing...", iCh);
    nmcDumpMappingTable((void *)dataBufAddr, true);

    // prepare request
    iReqEntry = GetFromFreeReqQ();

    REQ_ENTRY(iReqEntry)->reqType                       = REQ_TYPE_NAND;
    REQ_ENTRY(iReqEntry)->reqCode                       = REQ_CODE_WRITE;
    REQ_ENTRY(iReqEntry)->reqOpt.dataBufFormat          = REQ_OPT_DATA_BUF_ADDR;
    REQ_ENTRY(iReqEntry)->reqOpt.nandAddr               = REQ_OPT_NAND_ADDR_PHY_ORG;
    REQ_ENTRY(iReqEntry)->reqOpt.nandEcc                = REQ_OPT_NAND_ECC_ON;
    REQ_ENTRY(iReqEntry)->reqOpt.nandEccWarning         = REQ_OPT_NAND_ECC_WARNING_ON;
    REQ_ENTRY(iReqEntry)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
    REQ_ENTRY(iReqEntry)->reqOpt.blockSpace             = REQ_OPT_BLOCK_SPACE_TOTAL;
    REQ_ENTRY(iReqEntry)->dataBufInfo.addr              = dataBufAddr;
    REQ_ENTRY(iReqEntry)->nandInfo.physicalCh           = iCh;
    REQ_ENTRY(iReqEntry)->nandInfo.physicalWay          = nmcNewMappingLoc[iCh].iWay;
    REQ_ENTRY(iReqEntry)->nandInfo.physicalBlock        = NMC_NEW_MAPPING_PBLK_ON(iCh);
    REQ_ENTRY(iReqEntry)->nandInfo.physicalPage         = nmcNewMappingLoc[iCh].iPage;

    // issue request and wait for finish
    SelectLowLevelReqQ(iReqEntry);
    SyncAllLowLevelReqDone();

    // update file table
    title = NMC_CH_MAP(iCh)->post_info.title;
    type  = NMC_CH_MAP(iCh)->post_info.type;
    nmcFileTableUpdate(iCh, nmcNewMappingLoc[iCh], title, type);

    // update mapping table location for next file
    nmcUpdateNewMappingLoc(iCh);

    // reset PBA count
    NMC_CH_MAP_PRE_INFO(iCh)->count = 0;
}

static void nmcDumpCachedMappingTable()
{
    for (uint8_t iCh = 0; iCh < USER_CHANNELS; ++iCh)
    {
        pr_info("NMC: Cached Ch[%u] Mapping Table", iCh);
        nmcDumpMappingTable(NMC_CH_MAP(iCh), true);
        pr_info("-----------------------------------------------");
    }
}

static void nmcDumpNewMappingLoc(uint8_t iCh)
{
    pr_info("Ch[%u]: Update New Mapping Table Location:", iCh);
    pr_info("\t  Way[%u]", nmcNewMappingLoc[iCh].iWay);
    pr_info("\t  Dir[%u] (PBLK[%u])", nmcNewMappingLoc[iCh].iDir, NMC_NEW_MAPPING_PBLK_ON(iCh));
    pr_info("\t Page[%u]", nmcNewMappingLoc[iCh].iPage);
    pr_info("\t Data in channel", nmcNewMappingLoc[iCh].iPage);
}

void nmcDumpMappingTable(const void *addr, bool dumpBlks)
{
    const NMC_CH_MAPPING_TABLE *mapping = (NMC_CH_MAPPING_TABLE *)addr;

    char title[NMC_FILENAME_MAX_BYTES + 1] = {0};
    strncpy(title, mapping->post_info.title, NMC_FILENAME_MAX_BYTES);

    uint32_t magic;
    // magic = mapping->post_info.magic; // this have alignment issue...
    // memcpy(&magic, &mapping->post_info.magic, 4); // this also have alignment issue...
    magic = ((((uint16_t *)(&mapping->post_info.magic))[1]) << 16) + (*(uint16_t *)(&mapping->post_info.magic));

    bool warnNumBlks = mapping->pre_info.count > NMC_MAX_BLKS_PER_FC;

    pr_info("\t magic = %x", magic);
    pr_info("\t title = '%s'", title); // in case of non null-terminated string...
    pr_info("\t type  = %u", mapping->post_info.type);
    pr_info("\t count  = %u %s", mapping->pre_info.count, warnNumBlks ? "(too many blocks!!)" : "");
    pr_info("\t limit  = %u", mapping->post_info.limit);
    pr_info("\t Data in channel[%d]", mapping->pre_info.ch);

    // dump valid mapping tables with limited size
    if ((magic == NMC_MAPPING_DIR_BLK_USED) && dumpBlks)
    {
        union NMC_CH_MAPPING_PBA_LOC loc;

        NMC_BLOCK_TABLE_RESET(nmcTmpBlockTablePtr); // record blocks used by this file
        for (size_t i = 0; i < mapping->pre_info.count && i < NMC_MAX_BLKS_PER_FC; ++i)
        {
            loc = mapping->pba[i];

            if (NMC_TMP_USED_BLOCK_CW(0, loc.wayNo, loc.blkNo)->isUsed)
                continue; // do not dump repeated blocks

            NMC_TMP_USED_BLOCK_CW(0, loc.wayNo, loc.blkNo)->isUsed = true;
            if (loc.wayNo < USER_WAYS && loc.blkNo < NMC_MAX_BLKS_PER_FC)
                pr_debug("[%u] = W[%u].B[%u]", i, loc.wayNo, loc.blkNo);
            else
                pr_warn("[%u] = W[%u].B[%u] Out-of-range !!", i, loc.wayNo, loc.blkNo);
        }
    }
    pr_info("--------------------------------------------------------------------------------");
}

uintptr_t nmcReadMappingTable(uint8_t iCh, NMC_MAPPING_LOC loc)
{
    pr_debug("NMC: Reading Mapping Table on C[%u].W[%u].D[%u].P[%u]", iCh, loc.iWay, loc.iDir, loc.iPage);

    // prepare request and data buffer for reading pages
    uint32_t iReqEntry    = GetFromFreeReqQ();
    uintptr_t dataBufAddr = NMC_CH_MAP_BUFFER_ADDR(iCh);

    REQ_ENTRY(iReqEntry)->reqType                       = REQ_TYPE_NAND;
    REQ_ENTRY(iReqEntry)->reqCode                       = REQ_CODE_READ;
    REQ_ENTRY(iReqEntry)->reqOpt.dataBufFormat          = REQ_OPT_DATA_BUF_ADDR;
    REQ_ENTRY(iReqEntry)->reqOpt.nandAddr               = REQ_OPT_NAND_ADDR_PHY_ORG;
    REQ_ENTRY(iReqEntry)->reqOpt.nandEcc                = REQ_OPT_NAND_ECC_ON;
    REQ_ENTRY(iReqEntry)->reqOpt.nandEccWarning         = REQ_OPT_NAND_ECC_WARNING_ON;
    REQ_ENTRY(iReqEntry)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
    REQ_ENTRY(iReqEntry)->reqOpt.blockSpace             = REQ_OPT_BLOCK_SPACE_TOTAL;
    REQ_ENTRY(iReqEntry)->logicalSliceAddr              = REQ_OPT_BLOCK_SPACE_TOTAL;
    REQ_ENTRY(iReqEntry)->dataBufInfo.addr              = dataBufAddr;
    REQ_ENTRY(iReqEntry)->nandInfo.physicalCh           = iCh;
    REQ_ENTRY(iReqEntry)->nandInfo.physicalWay          = loc.iWay;
    REQ_ENTRY(iReqEntry)->nandInfo.physicalBlock        = NMC_MAPPING_DIR2PBLK(loc.iDir);
    REQ_ENTRY(iReqEntry)->nandInfo.physicalPage         = loc.iPage;

    // issue request and wait for finish
    SelectLowLevelReqQ(iReqEntry);
    SyncAllLowLevelReqDone();

    return NMC_CH_MAP_BUFFER_ADDR(iCh);
}
