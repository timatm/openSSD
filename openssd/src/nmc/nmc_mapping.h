#ifndef __OPENSSD_FW_NMC_MAPPING_H__
#define __OPENSSD_FW_NMC_MAPPING_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ftl_config.h"
#include "request_allocation.h"
#include "nmc/nmc_file_table.h"

#define NMC_CH_MAPPING_PREFIX_INFO_BYTES  (sizeof(uint16_t))
#define NMC_CH_MAPPING_POSTFIX_INFO_BYTES (1024 - NMC_CH_MAPPING_PREFIX_INFO_BYTES)
#define NMC_CH_MAPPING_INFO_BYTES         (NMC_CH_MAPPING_PREFIX_INFO_BYTES + NMC_CH_MAPPING_POSTFIX_INFO_BYTES)
#define NMC_CH_MAPPING_LIST_BYTES         (BYTES_PER_DATA_REGION_OF_PAGE - NMC_CH_MAPPING_INFO_BYTES)
#define NMC_CH_MAPPING_LIST_ENTRIES       (NMC_CH_MAPPING_LIST_BYTES / sizeof(uint16_t))

// count struct offsets for static assert
#define NMC_CH_MAPPING_PREFIX_INFO_OFFSET  0
#define NMC_CH_MAPPING_TABLE_OFFSET        NMC_CH_MAPPING_PREFIX_INFO_BYTES
#define NMC_CH_MAPPING_POSTFIX_INFO_OFFSET (NMC_CH_MAPPING_TABLE_OFFSET + NMC_CH_MAPPING_LIST_BYTES)

/**
 * @brief The max number of physical blocks used to store the NMC mappings.
 *
 * The fw statically allocates some blocks, called directory blocks, to store the mapping
 * tables on each channel.
 *
 * Each mapping table should occupy no more than 1 page, which means the max number of
 * blocks used by a file on a channel is 8192, and the max number of mapping tables the
 * fw can have is 256 x 8 x `NMC_MAPPING_DIR_PBLK_PER_DIE`.
 */
#define NMC_MAPPING_DIR_PBLK_PER_DIE 1
static const uint16_t NMC_MAPPING_DIR_PBLKS[NMC_MAPPING_DIR_PBLK_PER_DIE] = {1};
#define NMC_MAPPING_DIR2PBLK(iDir) (NMC_MAPPING_DIR_PBLKS[(iDir)])

#define NMC_MAPPING_DIR_BLK_USED   0x434D4E34 /* 4 bytes magic ("4NMC") for the used dir blocks */
#define NMC_MAPPING_DIR_BLK_UNUSED 0xFFFFFFFF /* all bits should all be 1 after being erased */

#define NMC_MAX_BLKS_PER_FC (TOTAL_BLOCKS_PER_DIE * USER_WAYS)

/**
 * @brief The mapping table for a NMC file on a flash channel.
 *
 * @note This structure will be used as data buffer when recovering directory block and
 * flushing the mapping table, make sure the structure size is the same as a flash page
 * size (data + spare).
 */
typedef union _NMC_CH_MAPPING_TABLE
{
#pragma pack(push, 1)
    uint8_t byte[BYTES_PER_SLICE];
    struct
    {
        /**
         * @brief The prefix nmc mapping table info.
         *
         * To speed up the inference process, all metadata of the mapping needed by the
         * hardware accelerator should be placed at the begin of the flash page.
         */
        union NMC_CH_MAPPING_PRE_INFO
        {
            uint8_t _pre_info_base[NMC_CH_MAPPING_PREFIX_INFO_BYTES];

            struct
            {
                uint8_t count; // NMC_CH_MAPPING_LIST_ENTRIES < 65535
                uint8_t ch; //modified original
            };
        } pre_info;

        /**
         * @brief The PBA list that records the blocks used by the file on the channel.
         *
         * Each flash channel has only 1 buffered mapping table, but since there are 8
         * dies in a flash channel, the block addresses should also contain its source
         * die number.
         */
        union NMC_CH_MAPPING_PBA_LOC
        {
            uint16_t addr;
            struct
            {
                uint16_t blkNo : 12;
                uint16_t wayNo : 4;
            };
        } pba[NMC_CH_MAPPING_LIST_ENTRIES];

        /**
         * @brief The postfix nmc mapping table info.
         *
         * To ensure efficient execution of the inference process, we should place mapping
         * metadata that is only needed by the firmware at the end of the flash page. This
         * prevents the hardware accelerator from having to skip unnecessary data during
         * inference, thereby reducing processing time and increasing performance.
         */
        union NMC_CH_MAPPING_POST_INFO
        {
            uint8_t _post_info_base[NMC_CH_MAPPING_POSTFIX_INFO_BYTES];
            struct
            {
                union
                {
                    uint8_t _[NMC_CH_MAPPING_POSTFIX_INFO_BYTES - NMC_FILENAME_MAX_BYTES];
                    struct
                    {
                        uint32_t magic; // magic number for checking
                        uint16_t limit; // NMC_CH_MAPPING_LIST_ENTRIES < 65535
                        union           // filetype, 16 bits
                        {
                            NMC_FILE_TYPES type;
                            uint16_t type_padding;
                        };
                    };
                };
                char title[NMC_FILENAME_MAX_BYTES]; // filename
            };
        } post_info;

        union
        {
            uint8_t _spare_base[BYTES_PER_SPARE_REGION_OF_SLICE];
            struct
            {
                // currently not used, just allocated for data buffer
            };
        } spare;
    };
#pragma pack(pop)
} NMC_CH_MAPPING_TABLE;

/**
 * @brief Mapping table used for recording the blocks allocated for a NMC application.
 *
 * To exploit the channel parallelism, the data will be distributed to the 8 channels,
 * therefore each channel need to maintain a mapping table to record the blocks used to
 * store the data on that channel.
 */
typedef struct _NMC_MAPPING_TABLE
{
    NMC_CH_MAPPING_TABLE nmcChMappingTable[USER_CHANNELS];
} NMC_MAPPING_TABLE;

typedef struct _NMC_BLOCK_TABLE
{
    struct
    {
        bool isUsed;
    } block[USER_DIES][TOTAL_BLOCKS_PER_DIE];
} NMC_BLOCK_TABLE;

/* -------------------------------------------------------------------------- */
/*                      public interfaces for NMC mapping                     */
/* -------------------------------------------------------------------------- */

void nmcInitMapping();
int nmcNewMapping(const char *filename, uint32_t filetype, uint32_t nblks);
int nmcFreeMapping(uint32_t cmdSlotTag, bool force);
void nmcRecordBlock(uint32_t iDie, uint32_t vba);
void nmcRecoverMappingDir();
bool nmcPhyBlockUsed(uint32_t iDie, uint32_t iBlk);
void nmcDumpMappingTable(const void *addr, bool dumpBlks);
uintptr_t nmcReadMappingTable(uint8_t iCh, NMC_MAPPING_LOC loc);

#endif /* __OPENSSD_FW_NMC_MAPPING_H__ */
