#ifndef __OPENSSD_FW_NMC_FILE_TABLE_H__
#define __OPENSSD_FW_NMC_FILE_TABLE_H__

#include "stdint.h"
#include "ftl_config.h"

#define NMC_FILENAME_MAX_BYTES     256
#define NMC_FILE_TABLE_MAX_ENTRIES 1024
#define NMC_FILE_TABLE_IDX_FAILED  UINT32_MAX

/**
 * @brief The supported file types for NMC application.
 */
typedef enum _NMC_FILE_TYPES
{
    NMC_FILE_TYPE_NONE       = 0,
    NMC_FILE_TYPE_MODEL_UNET = 1,
    NMC_FILE_TYPE_IMAGE_TIFF = 2,
} NMC_FILE_TYPES;

/**
 * @brief The physical address of the mapping table of a NMC file.
 */
typedef struct _NMC_MAPPING_LOC
{
    uint16_t iWay : 4;  // the way number of the page
    uint16_t iDir : 12; // the index of the dir pblk list
    uint16_t iPage;     // the next page used to persist mapping table
} NMC_MAPPING_LOC;

/**
 * @brief The cached NMC file info for searching mapping table locations.
 */
typedef struct _NMC_FILE_INFO
{
    char name[NMC_FILENAME_MAX_BYTES];
    NMC_FILE_TYPES type;
    NMC_MAPPING_LOC mapping[USER_CHANNELS];
} NMC_FILE_INFO;

#define NMC_FILE_TABLE_BYTES (sizeof(NMC_FILE_INFO) * NMC_FILE_TABLE_MAX_ENTRIES)

void nmcInitFileTable();
int nmcFileTableUpdate(uint32_t iCh, NMC_MAPPING_LOC loc, const char *name, NMC_FILE_TYPES type);
uint32_t nmcGetFileIdx(const char *filename);
NMC_FILE_INFO nmcSearchFileIdx(uint32_t iFile);
NMC_FILE_INFO nmcSearchFile(const char *filename);
bool nmcValidFilename(const char *filename);
void dumpFileInfo(NMC_FILE_INFO info);

#endif /* __OPENSSD_FW_NMC_FILE_TABLE_H__ */