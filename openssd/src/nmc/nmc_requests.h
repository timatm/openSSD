#ifndef __OPENSSD_FW_NMC_REQUESTS_H__
#define __OPENSSD_FW_NMC_REQUESTS_H__

#include <stdbool.h>
#include <stdint.h>

bool nmcRegisterNewMappingReqInit(uint32_t filetype, uint32_t nblks);
bool nmcRegisterNewMappingReqDone(uint32_t iReqEntry);
bool nmcRegisterInferenceReq(uint32_t iReqEntry);
void nmcReqScheduling();

#endif /* __OPENSSD_FW_NMC_REQUESTS_H__ */