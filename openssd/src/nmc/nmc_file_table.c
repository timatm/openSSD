#include "debug.h"
#include "string.h"

#include "memory_map.h"
#include "nmc/nmc_file_table.h"
#include "nmc/nmc_mapping.h"

typedef struct _NMC_FILE_TABLE
{
    NMC_FILE_INFO files[NMC_FILE_TABLE_MAX_ENTRIES];
} NMC_FILE_TABLE;

static uint32_t nmcNumFiles;
static NMC_FILE_TABLE *nmcFileTable;

#define NMC_FILE(iFile) (nmcFileTable->files[(iFile)])

/* -------------------------------------------------------------------------- */
/*                      public interface implementations                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize the file table (file info + file mapping loc) for inference.
 *
 * Different with the NMC mapping table, all flash channels share a file table. However,
 * since the flash page that is used to persist the mapping table of a file may differ,
 * each file table entry should maintain a `NMC_MAPPING_LOC` for each flash channel.
 *
 * @sa `NMC_FILE_INFO`.
 */
void nmcInitFileTable()
{
    // check file table address and size
    STATIC_ASSERT(sizeof(NMC_FILE_TABLE) == NMC_FILE_TABLE_BYTES);
    STATIC_ASSERT(NMC_FILE_TABLE_ADDR <= RESERVED1_END_ADDR);

    pr_info("NMC: Initializing NMC File Table...");

    nmcNumFiles  = 0;
    nmcFileTable = (NMC_FILE_TABLE *)NMC_FILE_TABLE_ADDR;

    memset(nmcFileTable, 0, NMC_FILE_TABLE_BYTES);
}

/**
 * @brief Update the mapping table info of the specified file.
 *
 * A file may call this several times because:
 *
 *  1. Each file have a mapping table on each flash channel.
 *  2. A file may have multiple versions.
 *
 * @param iCh The channel number for the file table of target channel.
 * @param loc The physical location info of the file mapping table.
 * @param fname The filename.
 * @param type The type of the file.
 * @return int 0 is returned on success, otherwise -1.
 */
int nmcFileTableUpdate(uint32_t iCh, NMC_MAPPING_LOC loc, const char *fname, NMC_FILE_TYPES type)
{
    uint32_t iFile;

    if (strlen(fname) > NMC_FILENAME_MAX_BYTES)
    {
        pr_error("NMC: Filename too long...");
        return -1;
    }

    iFile = nmcGetFileIdx(fname);
    if (iFile == NMC_FILE_TABLE_IDX_FAILED)
    {
        pr_info("NMC: Create new file info for: \"%s\"", fname);
        iFile = nmcNumFiles;
        ++nmcNumFiles;

        // don't forget to check size
        if (iFile > NMC_FILE_TABLE_MAX_ENTRIES)
        {
            pr_error("NMC: Too many files or ...");
            return -1;
        }

        // only need by new entry
        strcpy(NMC_FILE(iFile).name, fname);
    }
    pr_info("NMC: Ch[%u] Update file mapping location:", iCh);
    pr_info("\t filename: \"%s\"", fname);
    pr_info("\t Way[%u].Dir[%u].Page[%u]", loc.iWay, loc.iDir, loc.iPage);

    // for existing entry, only update location
    NMC_FILE(iFile).type         = type;
    NMC_FILE(iFile).mapping[iCh] = loc;

    return 0;
}

/**
 * @brief Try to get the index of the file table of the specified file.
 *
 * @param filename The filename of the file to search.
 * @return uint32_t The file table index of the target file, `NMC_FILE_TABLE_IDX_FAILED`
 * if not found.
 */
uint32_t nmcGetFileIdx(const char *filename)
{
    for (uint32_t iFile = 0; iFile < nmcNumFiles; ++iFile)
        if (!strncmp(NMC_FILE(iFile).name, filename, NMC_FILENAME_MAX_BYTES))
            return iFile;
    return NMC_FILE_TABLE_IDX_FAILED;
}

/**
 * @brief Try to get the file info of the specified file table entry.
 *
 * @param iFile The index of target file table entry.
 * @return NMC_FILE_INFO The file info of the target file. If the target file is not
 * found, the returned `NMC_FILE_INFO::type` will be `NMC_FILE_TYPE_NONE`.
 */
NMC_FILE_INFO nmcSearchFileIdx(uint32_t iFile)
{
    if (iFile >= NMC_FILE_TABLE_MAX_ENTRIES)
        return (NMC_FILE_INFO){.type = NMC_FILE_TYPE_NONE};
    return nmcFileTable->files[iFile];
}

/**
 * @brief Try to get the file info of the specified file.
 *
 * @param filename The filename of the file to search.
 * @return NMC_FILE_INFO The file info of the target file. If the target file is not
 * found, the returned `NMC_FILE_INFO::type` will be `NMC_FILE_TYPE_NONE`.
 */
NMC_FILE_INFO nmcSearchFile(const char *filename)
{
    pr_debug("NMC: Searching for file '%s'...", filename);
    for (uint32_t iFile = 0; iFile < nmcNumFiles; ++iFile)
    {
        pr_debug("File[%i] is '%s'", iFile, NMC_FILE(iFile).name);
        if (!strncmp(NMC_FILE(iFile).name, filename, NMC_FILENAME_MAX_BYTES))
            return NMC_FILE(iFile);
    }
    return (NMC_FILE_INFO){.type = NMC_FILE_TYPE_NONE};
}

bool nmcValidFilename(const char *filename)
{
    bool validFilename = true;

    for (uint32_t iChar = 0; filename[iChar] && iChar < NMC_FILENAME_MAX_BYTES; ++iChar)
    {
        switch (filename[iChar])
        {
        case '0' ... '9':
        case 'A' ... 'Z':
        case 'a' ... 'z':
        case '.':
            continue;
        default:
            pr_error("Unexpected Char[%u]: '0x%x'", iChar, filename[iChar]);
            validFilename = false;
        }
    }

    return validFilename;
}

/* -------------------------------------------------------------------------- */
/*                      internal utility implementations                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Dump the given NMC file info.
 *
 * @param info The target file info.
 */
void dumpFileInfo(NMC_FILE_INFO info)
{
    NMC_MAPPING_LOC loc;

    if (info.type == NMC_FILE_TYPE_NONE)
        pr_warn("Invalid NMC_FILE_INFO, cannot dump its file info...");
    else
    {
        pr_info("NMC: Location Table of file '%s' (type=%u) :", info.name, info.type);
        for (int iCh = 0; iCh < USER_CHANNELS; ++iCh)
        {
            loc = info.mapping[iCh];
            pr_info("\tC/W[%u/%u].PBlk[%u].Page[%u]", iCh, loc.iWay, NMC_MAPPING_DIR2PBLK(loc.iDir), loc.iPage);
        }
    }
}
