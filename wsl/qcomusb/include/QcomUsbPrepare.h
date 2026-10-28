/* SPDX-License-Identifier: MIT
 * SM8150/nabu device-mode bring-up that runs before the log module's
 * inherited-state qualification. Pure MMIO through callbacks, bounded work.
 */
#ifndef QCOM_USB_PREPARE_H
#define QCOM_USB_PREPARE_H
#include <stdint.h>
#include "QcomUsbSoc.h"

typedef struct {
    int  (*read32)(void *cookie, uint64_t address, uint32_t *value);
    int  (*write32)(void *cookie, uint64_t address, uint32_t value);
    void  *cookie;
    void (*delay_us)(void *cookie, uint32_t usec);
    uint64_t deadline;          /* absolute CNTPCT, 0 = no deadline */
} QCOM_PREPARE_PLATFORM;

typedef enum {
    QPREP_OK = 0,
    QPREP_ARG,          /* bad platform or profile lacks the PHY window */
    QPREP_DEADLINE,     /* clock/counter budget exhausted before finishing */
    QPREP_ID,           /* GSNPSID is not a supported DWC3 family */
    QPREP_CLOCKS,       /* a USB branch stayed gated (CBCR.CLK_OFF) */
    QPREP_GDSC,         /* GDSC never reported PWR_ON */
    QPREP_RESET,        /* a GCC reset did not release */
    QPREP_PHY,          /* femto HS PHY readback mismatch */
    QPREP_CORE          /* GCTL/DCTL did not reach device mode */
} QCOM_PREPARE_RESULT;

/* Stage numbers reported through failed_stage. */
enum {
    QPREP_STAGE_START = 0,
    QPREP_STAGE_CLOCKS = 1,
    QPREP_STAGE_ID = 2,
    QPREP_STAGE_PHY = 3,
    QPREP_STAGE_CORE = 4,
};

QCOM_PREPARE_RESULT QcomUsbPrepare(const QCOM_PREPARE_PLATFORM *platform,
                                   uint32_t *failed_stage, uint32_t *last_readback);
#endif
