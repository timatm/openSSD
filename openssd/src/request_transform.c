//////////////////////////////////////////////////////////////////////////////////
// request_transform.c for Cosmos+ OpenSSD
// Copyright (c) 2017 Hanyang University ENC Lab.
// Contributed by Yong Ho Song <yhsong@enc.hanyang.ac.kr>
//				  Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//			      Sangjin Lee <sjlee@enc.hanyang.ac.kr>
//
// This file is part of Cosmos+ OpenSSD.
//
// Cosmos+ OpenSSD is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Cosmos+ OpenSSD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Cosmos+ OpenSSD; see the file COPYING.
// If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Company: ENC Lab. <http://enc.hanyang.ac.kr>
// Engineer: Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// Project Name: Cosmos+ OpenSSD
// Design Name: Cosmos+ Firmware
// Module Name: Request Scheduler
// File Name: request_transform.c
//
// Version: v1.0.0
//
// Description:
//	 - transform request information
//   - check dependency between requests
//   - issue host DMA request to host DMA engine
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////

#include "xil_printf.h"
#include <assert.h>
#include "debug.h"

#include "nvme/nvme.h"
#include "nvme/host_lld.h"
#include "memory_map.h"
#include "ftl_config.h"
#include "request_transform.h"

#include "nmc/nmc_requests.h"

P_ROW_ADDR_DEPENDENCY_TABLE rowAddrDependencyTablePtr;

void InitDependencyTable()
{
    unsigned int blockNo, wayNo, chNo;
    rowAddrDependencyTablePtr = (P_ROW_ADDR_DEPENDENCY_TABLE)ROW_ADDR_DEPENDENCY_TABLE_ADDR;

    for (blockNo = 0; blockNo < MAIN_BLOCKS_PER_DIE; blockNo++)
    {
        for (wayNo = 0; wayNo < USER_WAYS; wayNo++)
        {
            for (chNo = 0; chNo < USER_CHANNELS; chNo++)
            {
                rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].permittedProgPage   = 0;
                rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedReadReqCnt   = 0;
                rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedEraseReqFlag = 0;
            }
        }
    }
}

/**
 * @brief Split NVMe command into slice requests.
 *
 * @note The unit of the given `startLba` and `nlb` is NVMe block, not NAND block.
 *
 * To get the starting LSA of this NVMe command, we need to divide the given `startLba` by
 * `NVME_BLOCKS_PER_SLICE` which indicates that how many NVMe blocks can be merged into a
 * slice request.
 *
 * To get the number of NAND blocks needed by this NVMe command, we should first align the
 * starting NVMe block address `startLba` to slice 0, then convert the ending NVMe block
 * address (`startLba` % `NVME_BLOCKS_PER_SLICE` + `requestedNvmeBlock`) to LSA, then the
 * result indicates the number of slice requests needed by this NVMe command.
 *
 * @note Accroding to the NVMe spec, NLB is a 0's based value, so we should increase the
 * `requestedNvmeBlock` by 1 to get the real number of NVMe blocks to be read/written by
 * this NVMe command.
 *
 * Now the address translation part is finished and we can start to split the NVMe command
 * into slice requests. The splitting process can be separated into 3 steps:
 *
 * 1. Fill the remaining NVMe blocks in first slice request (head)
 *
 *  Since the `startLba` may not perfectly align to the first NVMe block of first slice
 *  command, we should access the trailing N NVMe blocks in the first slice request, where
 *  N is the number of misaligned NVMe blocks in the first slice requests.
 *
 * 2. Generate slice requests for the aligned NVMe blocks (body)
 *
 *  General case. The number of the NVMe blocks to be filled by these slice requests is
 *  exactly `NVME_BLOCKS_PER_SLICE`. So here just simply use a loop to generate same slice
 *  requests.
 *
 * 3. Generate slice request for the remaining NVMe blocks (tail)
 *
 *  Similar to the first step, but here we need to access the first K NVMe blocks in the
 *  last slice request, where K is the number of remaining NVMe blocks in this slice
 *  request.
 *
 * @todo generalize the three steps
 *
 * @param cmdSlotTag @todo //TODO
 * @param startLba address of the first logical NVMe block to read/write.
 * @param nlb number of logical NVMe blocks to read/write.
 * @param cmdCode opcode of the given NVMe command.
 */
void ReqTransNvmeToSlice(unsigned int cmdSlotTag, unsigned int startLba, unsigned int nlb, unsigned int cmdCode)
{
    unsigned int reqSlotTag, requestedNvmeBlock, tempNumOfNvmeBlock, transCounter, tempLsa, loop, nvmeBlockOffset,
        nvmeDmaStartIndex, reqCode;

    requestedNvmeBlock = nlb + 1;
    transCounter       = 0;
    nvmeDmaStartIndex  = 0;
    tempLsa            = startLba / NVME_BLOCKS_PER_SLICE;
    loop               = ((startLba % NVME_BLOCKS_PER_SLICE) + requestedNvmeBlock) / NVME_BLOCKS_PER_SLICE;

    // translate the opcode for NVMe command into that for slice requests.
    switch (cmdCode)
    {
    case IO_NVM_WRITE:
        reqCode = REQ_CODE_WRITE;
        break;
    case IO_NVM_WRITE_PHY:
        reqCode = REQ_CODE_OCSSD_PHY_WRITE;
        break;
    case IO_NVM_READ:
        reqCode = REQ_CODE_READ;
        break;
    case IO_NVM_READ_PHY:
        reqCode = REQ_CODE_OCSSD_PHY_READ;
        break;

    /*
     * When the `NMC_SHORT_FILENAME` config is set to false, the firmware should generate
     * a NVMe Rx request to receive the filename from the host, and the received data
     * should not be written into flash memory.
     */
    case IO_NVM_NMC_ALLOC:
        reqCode            = REQ_CODE_NMC_NEW_MAPPING;
        loop               = 0;
        tempLsa            = LSA_NONE;
        startLba           = LSA_NONE & (~(NVME_BLOCKS_PER_SLICE - 1));         // avoid cross slice
        requestedNvmeBlock = (requestedNvmeBlock > 4) ? 4 : requestedNvmeBlock; // limit nlb
        break;

    /*
     * Each NMC_WRITE_TIFF request from the NMC host program should have the following
     * characteristics:
     *
     * 1. The data size is PAGE_SIZE x 8 (cuz the data should be distributed to 8 FCs)
     * 2. Based on 1, the starting LBA should be aligned to `NVME_BLOCKS_PER_SLICE`
     * 3. The NMC subsystem will change the interleaving policy, just translate to normal writes
     */
    case IO_NVM_NMC_WRITE:
        ASSERT(requestedNvmeBlock == (USER_CHANNELS * NVME_BLOCKS_PER_SLICE), "Unexpected NLB: %u", nlb);
        ASSERT(startLba % NVME_BLOCKS_PER_SLICE == 0, "Unaligned SLBA: %u", startLba);
        reqCode = REQ_CODE_WRITE;
        break;

    /*
     * For NVM_NMC_INFERENCE, since we only need to read the target filename from host
     * and the length of filename will be less than PAGE_SIZE, here we should prevent the
     * request from being divided into multiple slice requests.
     */
    case IO_NVM_NMC_INFERENCE:
        reqCode            = REQ_CODE_NMC_INFERENCE;
        loop               = 0;
        tempLsa            = LSA_NONE;
        startLba           = LSA_NONE & (~(NVME_BLOCKS_PER_SLICE - 1));         // avoid cross slice
        requestedNvmeBlock = (requestedNvmeBlock > 4) ? 4 : requestedNvmeBlock; // limit nlb
        break;

    default:
        ASSERT(0, "Unsupported cmdCode: %u!", cmdCode);
        break;
    }

    // first transform
    nvmeBlockOffset = (startLba % NVME_BLOCKS_PER_SLICE);
    if (loop)
        tempNumOfNvmeBlock = NVME_BLOCKS_PER_SLICE - nvmeBlockOffset;
    else
        tempNumOfNvmeBlock = requestedNvmeBlock;

    reqSlotTag = GetFromFreeReqQ();

    reqPoolPtr->reqPool[reqSlotTag].reqType                     = REQ_TYPE_SLICE;
    reqPoolPtr->reqPool[reqSlotTag].reqCode                     = reqCode;
    reqPoolPtr->reqPool[reqSlotTag].nvmeCmdSlotTag              = cmdSlotTag;
    reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr            = tempLsa;
    reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.startIndex      = nvmeDmaStartIndex;
    reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.nvmeBlockOffset = nvmeBlockOffset;
    reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.numOfNvmeBlock  = tempNumOfNvmeBlock;

    PutToSliceReqQ(reqSlotTag);

    tempLsa++;
    transCounter++;
    nvmeDmaStartIndex += tempNumOfNvmeBlock;

    // transform continue
    while (transCounter < loop)
    {
        nvmeBlockOffset    = 0;
        tempNumOfNvmeBlock = NVME_BLOCKS_PER_SLICE;

        reqSlotTag = GetFromFreeReqQ();

        reqPoolPtr->reqPool[reqSlotTag].reqType                     = REQ_TYPE_SLICE;
        reqPoolPtr->reqPool[reqSlotTag].reqCode                     = reqCode;
        reqPoolPtr->reqPool[reqSlotTag].nvmeCmdSlotTag              = cmdSlotTag;
        reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr            = tempLsa;
        reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.startIndex      = nvmeDmaStartIndex;
        reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.nvmeBlockOffset = nvmeBlockOffset;
        reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.numOfNvmeBlock  = tempNumOfNvmeBlock;

        PutToSliceReqQ(reqSlotTag);

        tempLsa++;
        transCounter++;
        nvmeDmaStartIndex += tempNumOfNvmeBlock;
    }

    // last transform
    nvmeBlockOffset    = 0;
    tempNumOfNvmeBlock = (startLba + requestedNvmeBlock) % NVME_BLOCKS_PER_SLICE;
    if ((tempNumOfNvmeBlock == 0) || (loop == 0))
        return;

    reqSlotTag = GetFromFreeReqQ();

    reqPoolPtr->reqPool[reqSlotTag].reqType                     = REQ_TYPE_SLICE;
    reqPoolPtr->reqPool[reqSlotTag].reqCode                     = reqCode;
    reqPoolPtr->reqPool[reqSlotTag].nvmeCmdSlotTag              = cmdSlotTag;
    reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr            = tempLsa;
    reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.startIndex      = nvmeDmaStartIndex;
    reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.nvmeBlockOffset = nvmeBlockOffset;
    reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.numOfNvmeBlock  = tempNumOfNvmeBlock;

    PutToSliceReqQ(reqSlotTag);
}

/**
 * @brief Clear the specified data buffer entry and sync dirty data if needed.
 *
 * In current implementation, the write request from host will not write the data directly
 * to the flash memory, but will cache the data in the data buffer and mark the entry as
 * dirty entry instead. Therefore, once the data buffer become full, the fw should check
 * whether the evicted entry is dirty and perform write request if needed before the entry
 * being evicted.
 *
 * @param originReqSlotTag the request entry index of the data buffer entry to be evicted.
 */
void EvictDataBufEntry(unsigned int originReqSlotTag)
{
    unsigned int reqSlotTag, virtualSliceAddr, dataBufEntry;

    dataBufEntry = REQ_ENTRY(originReqSlotTag)->dataBufInfo.entry;

    if (BUF_ENTRY(dataBufEntry)->dirty == DATA_BUF_DIRTY &&
        BUF_ENTRY(dataBufEntry)->dontCache == DATA_BUF_KEEP_CACHE)
    {
        if (BUF_ENTRY(dataBufEntry)->phyReq)
        {
            // FIXME: we should program a page once before that page being erased
            uint32_t iCh, iWay, iDie, iPBlk, iPage;

            virtualSliceAddr = BUF_LSA(dataBufEntry);
            iDie             = VSA2VDIE(virtualSliceAddr);
            iCh              = VDIE2PCH(iDie);
            iWay             = VDIE2PWAY(iDie);
            iPBlk            = VSA2VBLK(virtualSliceAddr);
            iPage            = VSA2VPAGE(virtualSliceAddr);

            reqSlotTag = GetFromFreeReqQ();

            REQ_ENTRY(reqSlotTag)->reqType                       = REQ_TYPE_NAND;
            REQ_ENTRY(reqSlotTag)->reqCode                       = REQ_CODE_WRITE;
            REQ_ENTRY(reqSlotTag)->nvmeCmdSlotTag                = REQ_ENTRY(originReqSlotTag)->nvmeCmdSlotTag;
            REQ_ENTRY(reqSlotTag)->logicalSliceAddr              = BUF_LSA(dataBufEntry);
            REQ_ENTRY(reqSlotTag)->reqOpt.dataBufFormat          = REQ_OPT_DATA_BUF_ENTRY;
            REQ_ENTRY(reqSlotTag)->reqOpt.nandAddr               = REQ_OPT_NAND_ADDR_PHY_ORG;
            REQ_ENTRY(reqSlotTag)->reqOpt.nandEcc                = REQ_OPT_NAND_ECC_ON;
            REQ_ENTRY(reqSlotTag)->reqOpt.nandEccWarning         = REQ_OPT_NAND_ECC_WARNING_ON;
            REQ_ENTRY(reqSlotTag)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
            REQ_ENTRY(reqSlotTag)->reqOpt.blockSpace             = REQ_OPT_BLOCK_SPACE_TOTAL;
            REQ_ENTRY(reqSlotTag)->dataBufInfo.entry             = dataBufEntry;
            REQ_ENTRY(reqSlotTag)->nandInfo.physicalCh           = iCh;
            REQ_ENTRY(reqSlotTag)->nandInfo.physicalWay          = iWay;
            REQ_ENTRY(reqSlotTag)->nandInfo.physicalBlock        = iPBlk;
            REQ_ENTRY(reqSlotTag)->nandInfo.physicalPage         = iPage;

            pr_info("Req[%u]: Write Ch[%u].Way[%u].PBlk[%u].Page[%u]", reqSlotTag, iCh, iWay, iPBlk, iPage);
        }
        else
        {
            reqSlotTag       = GetFromFreeReqQ();
            virtualSliceAddr = AddrTransWrite(BUF_LSA(dataBufEntry));

            REQ_ENTRY(reqSlotTag)->reqType                       = REQ_TYPE_NAND;
            REQ_ENTRY(reqSlotTag)->reqCode                       = REQ_CODE_WRITE;
            REQ_ENTRY(reqSlotTag)->nvmeCmdSlotTag                = REQ_ENTRY(originReqSlotTag)->nvmeCmdSlotTag;
            REQ_ENTRY(reqSlotTag)->logicalSliceAddr              = BUF_LSA(dataBufEntry);
            REQ_ENTRY(reqSlotTag)->reqOpt.dataBufFormat          = REQ_OPT_DATA_BUF_ENTRY;
            REQ_ENTRY(reqSlotTag)->reqOpt.nandAddr               = REQ_OPT_NAND_ADDR_VSA;
            REQ_ENTRY(reqSlotTag)->reqOpt.nandEcc                = REQ_OPT_NAND_ECC_ON;
            REQ_ENTRY(reqSlotTag)->reqOpt.nandEccWarning         = REQ_OPT_NAND_ECC_WARNING_ON;
            REQ_ENTRY(reqSlotTag)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
            REQ_ENTRY(reqSlotTag)->reqOpt.blockSpace             = REQ_OPT_BLOCK_SPACE_MAIN;
            REQ_ENTRY(reqSlotTag)->dataBufInfo.entry             = dataBufEntry;
            REQ_ENTRY(reqSlotTag)->nandInfo.virtualSliceAddr     = virtualSliceAddr;
        }

        UpdateDataBufEntryInfoBlockingReq(dataBufEntry, reqSlotTag);
        SelectLowLevelReqQ(reqSlotTag);

        BUF_ENTRY(dataBufEntry)->dirty = DATA_BUF_CLEAN;
    }
}

/**
 * @brief Generate and dispatch a flash read request for the given slice request.
 *
 * Before issuing NVMe Tx request and migration, we must read the target page into target
 * data buffer entry. To do this, we should create and issue a sub-request for flash read
 * operation.
 *
 * @warning In the original implementation, `nandInfo.virtualSliceAddr` was assigned after
 * calling the function `UpdateDataBufEntryInfoBlockingReq()`.
 *
 * @sa `ReqTransSliceToLowLevel()`
 *
 * @param originReqSlotTag the request pool entry index of the parent NVMe slice request.
 */
void DataReadFromNand(unsigned int originReqSlotTag)
{
    uint32_t reqSlotTag;

    if (REQ_CODE_IS(originReqSlotTag, REQ_CODE_READ))
    {
        uint32_t vsa = AddrTransRead(REQ_LSA(originReqSlotTag));

        if (vsa == VSA_FAIL)
        {
            /*
             * When the device is detected by the host, host will try to read some pages,
             * but the target pages may not have been programmed yet.
             *
             * To handle this and similar situations, the fw should just skip to read the
             * target page and return garbage data, instead of stucking here.
             */
            pr_warn("Req[%u]: No mapping info for LSA[%u]!", originReqSlotTag, REQ_LSA(originReqSlotTag));
            return;
        }

        /*
         * the request entry created by caller is only used for NVMe Tx/Rx, new request
         * entry is needed for flash read request.
         */
        reqSlotTag = GetFromFreeReqQ();

        REQ_ENTRY(reqSlotTag)->reqType                       = REQ_TYPE_NAND;
        REQ_ENTRY(reqSlotTag)->reqCode                       = REQ_CODE_READ;
        REQ_ENTRY(reqSlotTag)->nvmeCmdSlotTag                = REQ_ENTRY(originReqSlotTag)->nvmeCmdSlotTag;
        REQ_ENTRY(reqSlotTag)->logicalSliceAddr              = REQ_LSA(originReqSlotTag);
        REQ_ENTRY(reqSlotTag)->reqOpt.dataBufFormat          = REQ_OPT_DATA_BUF_ENTRY;
        REQ_ENTRY(reqSlotTag)->reqOpt.nandAddr               = REQ_OPT_NAND_ADDR_VSA;
        REQ_ENTRY(reqSlotTag)->reqOpt.nandEcc                = REQ_OPT_NAND_ECC_ON;
        REQ_ENTRY(reqSlotTag)->reqOpt.nandEccWarning         = REQ_OPT_NAND_ECC_WARNING_ON;
        REQ_ENTRY(reqSlotTag)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
        REQ_ENTRY(reqSlotTag)->reqOpt.blockSpace             = REQ_OPT_BLOCK_SPACE_MAIN;
        REQ_ENTRY(reqSlotTag)->dataBufInfo.entry             = REQ_ENTRY(originReqSlotTag)->dataBufInfo.entry;
        REQ_ENTRY(reqSlotTag)->nandInfo.virtualSliceAddr     = vsa;

        // dispatch request
        UpdateDataBufEntryInfoBlockingReq(REQ_ENTRY(reqSlotTag)->dataBufInfo.entry, reqSlotTag);
        SelectLowLevelReqQ(reqSlotTag);
    }
    else if (REQ_CODE_IS(originReqSlotTag, REQ_CODE_OCSSD_PHY_READ))
    {
        // for a phy read request, just use the lsa as vsa
        uint32_t iCh   = VDIE2PCH(VSA2VDIE(REQ_LSA(originReqSlotTag)));
        uint32_t iWay  = VDIE2PWAY(VSA2VDIE(REQ_LSA(originReqSlotTag)));
        uint32_t iPBlk = VSA2VBLK(REQ_LSA(originReqSlotTag));
        uint32_t iPage = VSA2VPAGE(REQ_LSA(originReqSlotTag));

        reqSlotTag = GetFromFreeReqQ();

        REQ_ENTRY(reqSlotTag)->reqType                       = REQ_TYPE_NAND;
        REQ_ENTRY(reqSlotTag)->reqCode                       = REQ_CODE_READ;
        REQ_ENTRY(reqSlotTag)->nvmeCmdSlotTag                = REQ_ENTRY(originReqSlotTag)->nvmeCmdSlotTag;
        REQ_ENTRY(reqSlotTag)->logicalSliceAddr              = REQ_LSA(originReqSlotTag);
        REQ_ENTRY(reqSlotTag)->reqOpt.dataBufFormat          = REQ_OPT_DATA_BUF_ENTRY;
        REQ_ENTRY(reqSlotTag)->reqOpt.nandAddr               = REQ_OPT_NAND_ADDR_PHY_ORG;
        REQ_ENTRY(reqSlotTag)->reqOpt.nandEcc                = REQ_OPT_NAND_ECC_ON;
        REQ_ENTRY(reqSlotTag)->reqOpt.nandEccWarning         = REQ_OPT_NAND_ECC_WARNING_ON;
        REQ_ENTRY(reqSlotTag)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
        REQ_ENTRY(reqSlotTag)->reqOpt.blockSpace             = REQ_OPT_BLOCK_SPACE_TOTAL;
        REQ_ENTRY(reqSlotTag)->dataBufInfo.entry             = REQ_ENTRY(originReqSlotTag)->dataBufInfo.entry;
        REQ_ENTRY(reqSlotTag)->nandInfo.physicalCh           = iCh;
        REQ_ENTRY(reqSlotTag)->nandInfo.physicalWay          = iWay;
        REQ_ENTRY(reqSlotTag)->nandInfo.physicalBlock        = iPBlk;
        REQ_ENTRY(reqSlotTag)->nandInfo.physicalPage         = iPage;

        pr_info("Req[%u]: Read Ch[%u].Way[%u].PBlk[%u].Page[%u]", reqSlotTag, iCh, iWay, iPBlk, iPage);

        // dispatch request
        UpdateDataBufEntryInfoBlockingReq(REQ_ENTRY(reqSlotTag)->dataBufInfo.entry, reqSlotTag);
        SelectLowLevelReqQ(reqSlotTag);
    }
    else
        ASSERT(0, "Req[%u]: Unexpected reqCode: %u", originReqSlotTag, REQ_ENTRY(originReqSlotTag)->reqCode);
}

/**
 * @brief Data Buffer Manager. Handle all the pending slice requests.
 *
 * This function will repeat the following steps until all the pending slice requests are
 * consumed:
 *
 * 1. Select a slice request from the slice request queue `sliceReqQ`.
 *
 * 2. Allocate a data buffer entry for the request and generate flash requests if needed.
 *
 *  @warning Why no need to modify `logicalSliceAddr` and generate flash request when
 *  buffer hit? data cache hit??
 *
 * 3. Generate NVMe transfer/receive request for read/write request.
 *
 *  @warning Why mark the data buffer dirty for write request?
 *
 * 4. Dispatch the transfer/receive request by calling `SelectLowLevelReqQ()`.
 *
 *
 * @note This function is currently only called after `handle_nvme_io_cmd()` during the
 * process of handling NVMe I/O commands in `nvme_main.c`.
 */
void ReqTransSliceToLowLevel()
{
    unsigned int reqSlotTag, dataBufEntry;

    // consume all pending slice requests in slice request queue
    while (sliceReqQ.headReq != REQ_SLOT_TAG_NONE)
    {
        // get the request pool entry index of the slice request
        reqSlotTag = GetFromSliceReqQ();
        if (reqSlotTag == REQ_SLOT_TAG_FAIL)
            return;

        /*
         * In current implementation, the data buffer to be used is determined on the
         * `logicalSliceAddr` of this request, so the data buffer may already be allocated
         * before and so we can simply reuse that data buffer.
         *
         * If the data buffer not exists, we must allocate a data buffer entry by calling
         * `AllocateDataBuf()` and initialize the newly created data buffer.
         */
        dataBufEntry = CheckDataBufHit(reqSlotTag);
        if (dataBufEntry != DATA_BUF_FAIL)
        {
            // data buffer hit
            REQ_ENTRY(reqSlotTag)->dataBufInfo.entry = dataBufEntry;
            pr_debug("Cache Hit! Use Buffer[%u] for Req[%u]", dataBufEntry, reqSlotTag);
        }
        else
        {
            // data buffer miss, allocate a new buffer entry
            dataBufEntry                             = AllocateDataBuf();
            REQ_ENTRY(reqSlotTag)->dataBufInfo.entry = dataBufEntry;
            pr_debug("Cache Miss! Allocate new Buffer[%u] for Req[%u]", dataBufEntry, reqSlotTag);

            // initialize the newly allocated data buffer entry for this request
            EvictDataBufEntry(reqSlotTag);
            BUF_ENTRY(dataBufEntry)->logicalSliceAddr = REQ_LSA(reqSlotTag);
            PutToDataBufHashList(dataBufEntry);

            /*
             * The allocated buffer will be used to store the data to be sent to host, or
             * received from the host. So before transfering the data to host, we need to
             * call the function `DataReadFromNand()` to read the desired data to buffer.
             */
            switch (REQ_ENTRY(reqSlotTag)->reqCode)
            {
            case REQ_CODE_OCSSD_PHY_READ:
            case REQ_CODE_READ:
                DataReadFromNand(reqSlotTag);
                break;

            case REQ_CODE_WRITE:
                // in case of not overwriting a whole page, read current page content for migration
                if (REQ_ENTRY(reqSlotTag)->nvmeDmaInfo.numOfNvmeBlock != NVME_BLOCKS_PER_SLICE)
                    // for read modify write
                    DataReadFromNand(reqSlotTag);
                break;

            default:
                break;
            }
        }

        // generate NVMe request by replacing the slice request entry directly
        switch (REQ_ENTRY(reqSlotTag)->reqCode)
        {

        // read data from host before writing target page
        case REQ_CODE_NMC_NEW_MAPPING:
        case REQ_CODE_NMC_INFERENCE:
        case REQ_CODE_OCSSD_PHY_WRITE:
        case REQ_CODE_WRITE:
            if (REQ_CODE_IS(reqSlotTag, REQ_CODE_OCSSD_PHY_WRITE))
                BUF_ENTRY(dataBufEntry)->phyReq = DATA_BUF_FOR_PHY_REQ;
            else
                BUF_ENTRY(dataBufEntry)->phyReq = DATA_BUF_FOR_LOG_REQ;

            if (REQ_CODE_IS(reqSlotTag, REQ_CODE_NMC_NEW_MAPPING))
            {
                BUF_ENTRY(dataBufEntry)->dirty = DATA_BUF_CLEAN; /* don't flush this */
                if (!nmcRegisterNewMappingReqDone(reqSlotTag))
                    continue;
            }
            else if (REQ_CODE_IS(reqSlotTag, REQ_CODE_NMC_INFERENCE))
            {
                ASSERT(nmcRegisterInferenceReq(reqSlotTag), "Too many inference request...");
                BUF_ENTRY(dataBufEntry)->dirty = DATA_BUF_CLEAN; /* don't flush this */
            }
            else
                BUF_ENTRY(dataBufEntry)->dirty = DATA_BUF_DIRTY;

            // generate NVMe Rx
            BUF_ENTRY(dataBufEntry)->dontCache = DATA_BUF_KEEP_CACHE;
            REQ_ENTRY(reqSlotTag)->reqCode     = REQ_CODE_RxDMA;
            break;

        // send data to host after target page being read
        case REQ_CODE_OCSSD_PHY_READ:
        case REQ_CODE_READ:
            if (REQ_CODE_IS(reqSlotTag, REQ_CODE_OCSSD_PHY_READ))
                BUF_ENTRY(dataBufEntry)->phyReq = DATA_BUF_FOR_PHY_REQ;
            else
                BUF_ENTRY(dataBufEntry)->phyReq = DATA_BUF_FOR_LOG_REQ;

            BUF_ENTRY(dataBufEntry)->dontCache = DATA_BUF_KEEP_CACHE;
            REQ_ENTRY(reqSlotTag)->reqCode     = REQ_CODE_TxDMA;
            break;

        default:
            ASSERT(0, "Unsupported reqCode: %u", REQ_ENTRY(reqSlotTag)->reqCode);
            break;
        }

        REQ_ENTRY(reqSlotTag)->reqType              = REQ_TYPE_NVME_DMA;
        REQ_ENTRY(reqSlotTag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;

        pr_debug("NVMe request (%X) generated:", REQ_ENTRY(reqSlotTag)->reqCode);
        pr_debug("\t reqCode = 0x%x", REQ_ENTRY(reqSlotTag)->reqCode);
        pr_debug("\t dataBufEntry = 0x%x", REQ_ENTRY(reqSlotTag)->dataBufInfo.entry);

        UpdateDataBufEntryInfoBlockingReq(dataBufEntry, reqSlotTag);
        SelectLowLevelReqQ(reqSlotTag);
    }
}

/**
 * @brief Check if this request has the buffer dependency problem.
 *
 * Requests that share the same data buffer entry must be executed in correct order, and
 * the execution order is identical to the order of the request entry index appended to
 * the blocking request queue, so we can simply check if the previous request in the
 * blocking request queue exists.
 *
 * @sa `UpdateDataBufEntryInfoBlockingReq()` and `DATA_BUF_ENTRY`.
 *
 * @param reqSlotTag the request pool entry index of the request to be checked
 * @return unsigned int 1 for pass, 0 for blocked
 */
unsigned int CheckBufDep(unsigned int reqSlotTag)
{
    if (reqPoolPtr->reqPool[reqSlotTag].prevBlockingReq == REQ_SLOT_TAG_NONE)
        return BUF_DEPENDENCY_REPORT_PASS;
    else
        return BUF_DEPENDENCY_REPORT_BLOCKED;
}

/**
 * @brief Check if this NAND request has the row address dependency problem.
 *
 * First, the NAND request should already be assigned a VSA, and we need to translate the
 * VSA info physical info.
 *
 * Now we the physical info of the target address of the specified request, but different
 * type of request have different dependency problem:
 *
 * - For a write request:
 *
 *      In current implementation, pages in the same block will be allocated sequentially,
 *      and thus here we should block all the write requests whose target PBN is not the
 *      expected page of the current working block on the target die.
 *
 * - For a erase request:
 *
 *      Before erasing a block, we must ensure that there is no pending read request that
 *      require access to any of the pages within the target block.
 *
 *      @warning What is the use of `programmedPageCnt`?
 *
 * - For a read request:
 *
 *      Before performing read operation the a block, we should ensure the pending write
 *      and erase requests are already finished.
 *
 * @todo Why the address info of the specified request must be VSA.
 *
 * @warning This function may update the count of corresponding block info, but won't add
 * or remove the given request to or from the row address dependency queue, so use this
 * function carefully.
 *
 * @sa `SyncReleaseEraseReq()`, `UpdateRowAddrDepTableForBufBlockedReq()`.
 *
 * @param reqSlotTag The request pool entry index of the request to be checked.
 * @param checkRowAddrDepOpt Increased or decreased the count of block info.
 * @return unsigned int The check result, 1 for pass, 0 for blocked.
 */
unsigned int CheckRowAddrDep(unsigned int reqSlotTag, unsigned int checkRowAddrDepOpt)
{
    unsigned int dieNo, chNo, wayNo, blockNo, pageNo;

    if (reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr == REQ_OPT_NAND_ADDR_VSA)
    {
        dieNo   = Vsa2VdieTranslation(reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr);
        chNo    = Vdie2PchTranslation(dieNo);
        wayNo   = Vdie2PwayTranslation(dieNo);
        blockNo = Vsa2VblockTranslation(reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr);
        pageNo  = Vsa2VpageTranslation(reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr);
    }
    else
        assert(!"[WARNING] Not supported reqOpt-nandAddress [WARNING]");

    if (reqPoolPtr->reqPool[reqSlotTag].reqCode == REQ_CODE_READ)
    {
        if (checkRowAddrDepOpt == ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT)
        {
            // release the blocked erase request on the target block
            if (rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedEraseReqFlag)
                SyncReleaseEraseReq(chNo, wayNo, blockNo);

            // already programed
            if (pageNo <= rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].permittedProgPage) // TEST
                return ROW_ADDR_DEPENDENCY_REPORT_PASS;

            pr_debug("READ Req[%u] (VSA[%u]) is blocked:", reqSlotTag, REQ_VSA(reqSlotTag));
            pr_debug("\tpageNo = %u", pageNo);
            pr_debug("\tC/W/B[%u/%u/%u].permittedProgPage = %u", chNo, wayNo, blockNo,
                     rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].permittedProgPage);

            rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedReadReqCnt++;
        }
        else if (checkRowAddrDepOpt == ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE)
        {
            if (pageNo < rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].permittedProgPage)
            {
                rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedReadReqCnt--;
                return ROW_ADDR_DEPENDENCY_REPORT_PASS;
            }
        }
        else
            assert(!"[WARNING] Not supported checkRowAddrDepOpt [WARNING]");
    }
    else if (reqPoolPtr->reqPool[reqSlotTag].reqCode == REQ_CODE_WRITE)
    {
        pr_debug("WRITE Req[%u] (VSA[%u]) row address dependency:", reqSlotTag, REQ_VSA(reqSlotTag));
        if (pageNo == rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].permittedProgPage)
        {
            rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].permittedProgPage++;
            pr_debug("PASS, permittedProgPage = %u", ROW_ADDR_DEP_ENTRY(chNo, wayNo, blockNo)->permittedProgPage);
            return ROW_ADDR_DEPENDENCY_REPORT_PASS;
        }
        pr_debug("BLOCKED, permittedProgPage = %u, pageNo = %u",
                 ROW_ADDR_DEP_ENTRY(chNo, wayNo, blockNo)->permittedProgPage, pageNo);
    }
    else if (reqPoolPtr->reqPool[reqSlotTag].reqCode == REQ_CODE_ERASE)
    {
        // FIXME: why check this
        if (rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].permittedProgPage ==
            reqPoolPtr->reqPool[reqSlotTag].nandInfo.programmedPageCnt)
        {
            if (rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedReadReqCnt == 0)
            {
                rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].permittedProgPage   = 0;
                rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedEraseReqFlag = 0;

                return ROW_ADDR_DEPENDENCY_REPORT_PASS;
            }
        }
        else
        {
            pr_debug("Req[%u] and C/W/B[%u/%u/%u] have diff programmedPageCnt", reqSlotTag, chNo, wayNo, blockNo);
        }

        if (checkRowAddrDepOpt == ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT)
            rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedEraseReqFlag = 1;
        else if (checkRowAddrDepOpt == ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE)
        {
            // pass, go to return
        }
        else
            assert(!"[WARNING] Not supported checkRowAddrDepOpt [WARNING]");
    }
    else
        assert(!"[WARNING] Not supported reqCode [WARNING]");

    return ROW_ADDR_DEPENDENCY_REPORT_BLOCKED;
}

/**
 * @brief Update the dependency info and dispatch the request if possible.
 *
 * This function will update the data buffer and row address dependency info of the given
 * request. If the given request is READ request and have no dependency problem, it will
 * be dispatched in this function.
 *
 * // FIXME: why only update buf dep for READ request? why ignore WRITE request?
 *
 * @warning Unlike `CheckRowAddrDep()`, this function may insert the given READ request
 * into `blockedByRowAddrDepReqQ` and update relevant blocking info directly.
 *
 * @sa `CheckRowAddrDep()`, `SelectLowLevelReqQ()`.
 *
 * @param reqSlotTag Request entry index of the request to be checked.
 * @return unsigned int The result of dependency check.
 */
unsigned int UpdateRowAddrDepTableForBufBlockedReq(unsigned int reqSlotTag)
{
    unsigned int dieNo, chNo, wayNo, blockNo, pageNo, bufDepCheckReport;

    if (reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr == REQ_OPT_NAND_ADDR_VSA)
    {
        dieNo   = Vsa2VdieTranslation(reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr);
        chNo    = Vdie2PchTranslation(dieNo);
        wayNo   = Vdie2PwayTranslation(dieNo);
        blockNo = Vsa2VblockTranslation(reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr);
        pageNo  = Vsa2VpageTranslation(reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr);
    }
    else
        assert(!"[WARNING] Not supported reqOpt-nandAddress [WARNING]");

    if (reqPoolPtr->reqPool[reqSlotTag].reqCode == REQ_CODE_READ)
    {
        if (rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedEraseReqFlag)
        {
            // release the blocked erase request on the target block
            SyncReleaseEraseReq(chNo, wayNo, blockNo);

            // check if this request is still blocked by buffer dependency
            bufDepCheckReport = CheckBufDep(reqSlotTag);
            if (bufDepCheckReport == BUF_DEPENDENCY_REPORT_PASS)
            {
                // check row address dependency problem
                if (pageNo < rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].permittedProgPage)
                    PutToNandReqQ(reqSlotTag, chNo, wayNo);
                else
                {
                    rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedReadReqCnt++;
                    PutToBlockedByRowAddrDepReqQ(reqSlotTag, chNo, wayNo);
                }

                return ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_SYNC;
            }
        }
        // still blocked by data buffer
        rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedReadReqCnt++;
    }
    else if (reqPoolPtr->reqPool[reqSlotTag].reqCode == REQ_CODE_ERASE)
        rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedEraseReqFlag = 1;

    return ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_DONE;
}

/**
 * @brief Dispatch given NVMe/NAND request to corresponding request queue.
 *
 * This function is in charge of issuing the given NVMe/NAND request. But before issuing
 * the request, we should first make sure that this request is safe to be issued.
 *
 * We first need to check whether this request is blocked by any other request that uses
 * the same data buffer (check `UpdateDataBufEntryInfoBlockingReq()` for details).
 *
 *  - If the request is not blocked by the blocking request queue, we can start issuing
 *    the request now, but NVMe/NAND request have different process:
 *
 *    - For a NVNe DMA request (Tx from/to data buffer to/from host), we can just issue
 *      the request and wait for completion.
 *
 *    - For a NAND request, we must do something before issuing the request:
 *
 *      However, for NAND requests, since there may be some dependency problems between
 *      the requests (e.g., ERASE cannot be simply reordered before READ), we must check
 *      this kind of dependency problems (called "row address dependency" here) before
 *      dispatching the NAND requests by using the function `CheckRowAddrDep()`.
 *
 *      Once it was confirmed to have no row address dependency problem on this request,
 *      the request can then be dispatched; otherwise, the request should be blocked and
 *      inserted to the row address dependency queue.
 *
 *      @note In current implementation, the address format of the request to be check
 *      must be VSA. So, for requests that using the physical address, the check option
 *      should be set to `REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK`.
 *
 *  - If the request is blocked by data buffer dependency
 *
 *      The fw will try to recheck the data buffer dependency problem and release the
 *      request if possible, by calling `UpdateRowAddrDepTableForBufBlockedReq()`, which
 *      is similar to `CheckRowAddrDep()`.
 *
 * @sa `CheckRowAddrDep()`, `UpdateDataBufEntryInfoBlockingReq()`, `NAND_INFO`.
 *
 * @param reqSlotTag the request pool index of the given request.
 */
void SelectLowLevelReqQ(unsigned int reqSlotTag)
{
    unsigned int dieNo, chNo, wayNo, bufDepCheckReport, rowAddrDepCheckReport, rowAddrDepTableUpdateReport;

    bufDepCheckReport = CheckBufDep(reqSlotTag);

    if (bufDepCheckReport == BUF_DEPENDENCY_REPORT_PASS)
    {
        if (reqPoolPtr->reqPool[reqSlotTag].reqType == REQ_TYPE_NVME_DMA)
        {
            IssueNvmeDmaReq(reqSlotTag);
            PutToNvmeDmaReqQ(reqSlotTag);
        }
        else if (reqPoolPtr->reqPool[reqSlotTag].reqType == REQ_TYPE_NAND)
        {
            // get physical organization info from VSA
            if (reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr == REQ_OPT_NAND_ADDR_VSA)
            {
                dieNo = Vsa2VdieTranslation(reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr);
                chNo  = Vdie2PchTranslation(dieNo);
                wayNo = Vdie2PwayTranslation(dieNo);
            }
            // if the physical organization info is already specified, use it without translating
            else if (reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr == REQ_OPT_NAND_ADDR_PHY_ORG)
            {
                chNo  = reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalCh;
                wayNo = reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalWay;
            }
            else
                assert(!"[WARNING] Not supported reqOpt-nandAddress [WARNING]");

            // check row address dependency problem before dispatching
            if (reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck == REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK)
            {
                // NOTE: VSA translation in `CheckRowAddrDep()` could be skipped
                rowAddrDepCheckReport = CheckRowAddrDep(reqSlotTag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT);

                if (rowAddrDepCheckReport == ROW_ADDR_DEPENDENCY_REPORT_PASS)
                    PutToNandReqQ(reqSlotTag, chNo, wayNo);
                else if (rowAddrDepCheckReport == ROW_ADDR_DEPENDENCY_REPORT_BLOCKED)
                    PutToBlockedByRowAddrDepReqQ(reqSlotTag, chNo, wayNo);
                else
                    assert(!"[WARNING] Not supported report [WARNING]");
            }
            else if (reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck ==
                     REQ_OPT_ROW_ADDR_DEPENDENCY_NONE)
                PutToNandReqQ(reqSlotTag, chNo, wayNo);
            else
                assert(!"[WARNING] Not supported reqOpt [WARNING]");
        }
        else
            assert(!"[WARNING] Not supported reqType [WARNING]");
    }
    else if (bufDepCheckReport == BUF_DEPENDENCY_REPORT_BLOCKED)
    {
        pr_debug("Req[%u]: Blocked by buffer dependency...", reqSlotTag);

        if (reqPoolPtr->reqPool[reqSlotTag].reqType == REQ_TYPE_NAND)
            if (reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck == REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK)
            {
                // update row addr dep info and insert to `blockedByRowAddrDepReqQ` if needed
                rowAddrDepTableUpdateReport = UpdateRowAddrDepTableForBufBlockedReq(reqSlotTag);

                if (rowAddrDepTableUpdateReport == ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_DONE)
                {
                    // no row addr dep problem, so put to blockedByBufDepReqQ
                }
                else if (rowAddrDepTableUpdateReport == ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_SYNC)
                    return;
                else
                    assert(!"[WARNING] Not supported report [WARNING]");
            }

        PutToBlockedByBufDepReqQ(reqSlotTag);
    }
    else
        assert(!"[WARNING] Not supported report [WARNING]");
}

/**
 * @brief Pop the specified request from the buffer dependency queue.
 *
 * In the current implementation, this function is only called after the specified request
 * entry is moved to the free request queue, which means that the previous request has
 * released the data buffer entry it occupied. Therefore, we now need to update the
 * relevant information about the data buffer dependency.
 *
 * @warning Only the NAND requests with VSA can use this function.
 *
 * @warning Since the struct `DATA_BUF_ENTRY` maintains only the tail of blocked requests,
 * the specified request should be the head of blocked requests to ensure that the request
 * order is not messed up.
 *
 * @sa `UpdateDataBufEntryInfoBlockingReq()`, `CheckBufDep()`, `SSD_REQ_FORMAT`.
 *
 * @param reqSlotTag The request entry index of the given request
 */
void ReleaseBlockedByBufDepReq(unsigned int reqSlotTag)
{
    unsigned int targetReqSlotTag, dieNo, chNo, wayNo, rowAddrDepCheckReport;

    // split the blocking request queue into 2 parts at `reqSlotTag`
    targetReqSlotTag = REQ_SLOT_TAG_NONE;
    if (reqPoolPtr->reqPool[reqSlotTag].nextBlockingReq != REQ_SLOT_TAG_NONE)
    {
        targetReqSlotTag                                      = reqPoolPtr->reqPool[reqSlotTag].nextBlockingReq;
        reqPoolPtr->reqPool[targetReqSlotTag].prevBlockingReq = REQ_SLOT_TAG_NONE;
        reqPoolPtr->reqPool[reqSlotTag].nextBlockingReq       = REQ_SLOT_TAG_NONE;
    }

    // reset blocking request queue if it is the last request blocked by the buffer dependency
    if (reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat == REQ_OPT_DATA_BUF_ENTRY)
    {
        if (dataBufMapPtr->dataBuf[reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry].blockingReqTail ==
            reqSlotTag)
            dataBufMapPtr->dataBuf[reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry].blockingReqTail =
                REQ_SLOT_TAG_NONE;
    }
    else if (reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat == REQ_OPT_DATA_BUF_TEMP_ENTRY)
    {
        if (tempDataBufMapPtr->tempDataBuf[reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry].blockingReqTail ==
            reqSlotTag)
            tempDataBufMapPtr->tempDataBuf[reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry].blockingReqTail =
                REQ_SLOT_TAG_NONE;
    }

    /*
     * the specified request is released, so if its next request is only blocked by data
     * buffer, it can be released now.
     */
    if ((targetReqSlotTag != REQ_SLOT_TAG_NONE) &&
        (reqPoolPtr->reqPool[targetReqSlotTag].reqQueueType == REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP))
    {
        SelectiveGetFromBlockedByBufDepReqQ(targetReqSlotTag);

        if (reqPoolPtr->reqPool[targetReqSlotTag].reqType == REQ_TYPE_NVME_DMA)
        {
            IssueNvmeDmaReq(targetReqSlotTag);
            PutToNvmeDmaReqQ(targetReqSlotTag);
        }
        else if (reqPoolPtr->reqPool[targetReqSlotTag].reqType == REQ_TYPE_NAND)
        {
            if (reqPoolPtr->reqPool[targetReqSlotTag].reqOpt.nandAddr == REQ_OPT_NAND_ADDR_VSA)
            {
                dieNo = Vsa2VdieTranslation(reqPoolPtr->reqPool[targetReqSlotTag].nandInfo.virtualSliceAddr);
                chNo  = Vdie2PchTranslation(dieNo);
                wayNo = Vdie2PwayTranslation(dieNo);
            }
            else
                assert(!"[WARNING] Not supported reqOpt-nandAddress [WARNING]");

            // check the row address dependency if needed
            if (reqPoolPtr->reqPool[targetReqSlotTag].reqOpt.rowAddrDependencyCheck ==
                REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK)
            {
                rowAddrDepCheckReport = CheckRowAddrDep(targetReqSlotTag, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE);

                if (rowAddrDepCheckReport == ROW_ADDR_DEPENDENCY_REPORT_PASS)
                    PutToNandReqQ(targetReqSlotTag, chNo, wayNo);
                else if (rowAddrDepCheckReport == ROW_ADDR_DEPENDENCY_REPORT_BLOCKED)
                    PutToBlockedByRowAddrDepReqQ(targetReqSlotTag, chNo, wayNo);
                else
                    assert(!"[WARNING] Not supported report [WARNING]");
            }
            else if (reqPoolPtr->reqPool[targetReqSlotTag].reqOpt.rowAddrDependencyCheck ==
                     REQ_OPT_ROW_ADDR_DEPENDENCY_NONE)
                PutToNandReqQ(targetReqSlotTag, chNo, wayNo);
            else
                assert(!"[WARNING] Not supported reqOpt [WARNING]");
        }
    }
}

/**
 * @brief Update the row address dependency of all the requests on the specified die.
 *
 * Traverse the `blockedByRowAddrDepReqQ` of the specified die, and then recheck the row
 * address dependency for all the requests on that die. When a request is found that it
 * can pass the dependency check, it will be dispatched (move to the NAND request queue).
 *
 * By updating the row address dependency info, some requests on the target die may be
 * released.
 *
 * @sa `CheckRowAddrDep()`.
 *
 * @param chNo The channel number of the specified die.
 * @param wayNo The way number of the specified die.
 */
void ReleaseBlockedByRowAddrDepReq(unsigned int chNo, unsigned int wayNo)
{
    unsigned int reqSlotTag, nextReq, rowAddrDepCheckReport;

    reqSlotTag = blockedByRowAddrDepReqQ[chNo][wayNo].headReq;

    while (reqSlotTag != REQ_SLOT_TAG_NONE)
    {
        nextReq = reqPoolPtr->reqPool[reqSlotTag].nextReq;

        if (reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck == REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK)
        {
            rowAddrDepCheckReport = CheckRowAddrDep(reqSlotTag, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE);

            if (rowAddrDepCheckReport == ROW_ADDR_DEPENDENCY_REPORT_PASS)
            {
                SelectiveGetFromBlockedByRowAddrDepReqQ(reqSlotTag, chNo, wayNo);
                PutToNandReqQ(reqSlotTag, chNo, wayNo);
            }
            else if (rowAddrDepCheckReport == ROW_ADDR_DEPENDENCY_REPORT_BLOCKED)
            {
                // pass, go to while loop
            }
            else
                assert(!"[WARNING] Not supported report [WARNING]");
        }
        else
            assert(!"[WARNING] Not supported reqOpt [WARNING]");

        reqSlotTag = nextReq;
    }
}

/**
 * @brief Allocate data buffer for the specified DMA request and inform the controller.
 *
 * This function is used for issuing a new DMA request, the DMA procedure can be split
 * into 2 steps:
 *
 * 1. Prepare a buffer based on the member `dataBufFormat` of the specified DMA request
 * 2. Inform NVMe controller
 *
 *      For a DMA request, it might want to rx/tx a data whose size is larger than 4K
 *      which is the NVMe block size, so the firmware need to inform the NVMe controller
 *      for each NVMe block.
 *
 *      The tail reg of the DMA queue will be updated during the `set_auto_rx_dma()` and
 *      `set_auto_tx_dma()`, so we need to update the `nvmeDmaInfo.reqTail` after issuing
 *      the DMA request.
 *
 * @warning For a DMA request, the buffer address generated by `GenerateDataBufAddr()` is
 * chosen based on the `REQ_OPT_DATA_BUF_ENTRY`, however, since the size of a data entry
 * is `BYTES_PER_DATA_REGION_OF_SLICE` (default 4), will the data buffer used by the DMA
 * request overlap with other requests' data buffer if the `numOfNvmeBlock` of the DMA
 * request is larger than `NVME_BLOCKS_PER_PAGE`?
 *
 * @param reqSlotTag the request pool index of the given request.
 */
void IssueNvmeDmaReq(unsigned int reqSlotTag)
{
    unsigned int devAddr, dmaIndex, numOfNvmeBlock;

    dmaIndex       = reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.startIndex;
    devAddr        = GenerateDataBufAddr(reqSlotTag);
    numOfNvmeBlock = 0;

    if (reqPoolPtr->reqPool[reqSlotTag].reqCode == REQ_CODE_RxDMA)
    {
        while (numOfNvmeBlock < reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.numOfNvmeBlock)
        {
            set_auto_rx_dma(reqPoolPtr->reqPool[reqSlotTag].nvmeCmdSlotTag, dmaIndex, devAddr,
                            NVME_COMMAND_AUTO_COMPLETION_ON);

            numOfNvmeBlock++;
            dmaIndex++;
            devAddr += BYTES_PER_NVME_BLOCK;
        }
        reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.reqTail     = g_hostDmaStatus.fifoTail.autoDmaRx;
        reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.overFlowCnt = g_hostDmaAssistStatus.autoDmaRxOverFlowCnt;
    }
    else if (reqPoolPtr->reqPool[reqSlotTag].reqCode == REQ_CODE_TxDMA)
    {
        while (numOfNvmeBlock < reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.numOfNvmeBlock)
        {
            set_auto_tx_dma(reqPoolPtr->reqPool[reqSlotTag].nvmeCmdSlotTag, dmaIndex, devAddr,
                            NVME_COMMAND_AUTO_COMPLETION_ON);

            numOfNvmeBlock++;
            dmaIndex++;
            devAddr += BYTES_PER_NVME_BLOCK;
        }
        reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.reqTail     = g_hostDmaStatus.fifoTail.autoDmaTx;
        reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.overFlowCnt = g_hostDmaAssistStatus.autoDmaTxOverFlowCnt;
    }
    else
        assert(!"[WARNING] Not supported reqCode [WARNING]");
}

void CheckDoneNvmeDmaReq()
{
    unsigned int reqSlotTag, prevReq;
    unsigned int rxDone, txDone;

    reqSlotTag = nvmeDmaReqQ.tailReq;
    rxDone     = 0;
    txDone     = 0;

    while (reqSlotTag != REQ_SLOT_TAG_NONE)
    {
        prevReq = reqPoolPtr->reqPool[reqSlotTag].prevReq;

        if (reqPoolPtr->reqPool[reqSlotTag].reqCode == REQ_CODE_RxDMA)
        {
            if (!rxDone)
                rxDone = check_auto_rx_dma_partial_done(reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.reqTail,
                                                        reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.overFlowCnt);

            if (rxDone)
                SelectiveGetFromNvmeDmaReqQ(reqSlotTag);
        }
        else
        {
            if (!txDone)
                txDone = check_auto_tx_dma_partial_done(reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.reqTail,
                                                        reqPoolPtr->reqPool[reqSlotTag].nvmeDmaInfo.overFlowCnt);

            if (txDone)
                SelectiveGetFromNvmeDmaReqQ(reqSlotTag);
        }

        reqSlotTag = prevReq;
    }
}
