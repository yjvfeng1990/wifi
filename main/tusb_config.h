#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_OS             OPT_OS_FREERTOS
#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_ECM_RNDIS       0
#define CFG_TUD_NCM             1

#define CFG_TUD_NCM_IN_NTB_MAX_SIZE    3200
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE   3200

#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

#ifdef __cplusplus
}
#endif