/* SPDX-License-Identifier: MIT
 * USB stream pass-through through the firmware's bypass context bank.
 *
 * On this firmware a TYPE=BYPASS S2CR reads back as FAULT (evidence: the sheng
 * port's SmmuBypass.h and the board's own UFS takeover in
 * drivers/NativeUfs/SmmuTakeover.h). The working configuration is a context
 * bank with S1 translation off (SCTLR=0), CBAR.TYPE=S1_TRANS_S2_BYPASS
 * (0x10000) and CBA2R.VA64=1; the stream selects it with an ordinary
 * TYPE=TRANSLATE S2CR holding the bank number. Writes to S2CR/CBAR/SCTLR on
 * SM8150 do not read back the written value, so nothing here treats a
 * mismatched readback as failure - the caller verifies with real DMA (host
 * reset / event ring), and every register we changed is saved and written back
 * by QcomUsbBypassRestore().
 */
#ifndef QCOM_USB_BYPASS_H
#define QCOM_USB_BYPASS_H

#include "QcomUsbQualify.h"

typedef struct {
  QUSB_U32 active, phase;     /* phase 1 = commit, 2 = restore */
  QUSB_U32 slot, bank, pages;
  QUSB_U32 original_cr0, original_s2cr, original_smr;
  QUSB_U32 saved_sctlr, saved_cbar, saved_cba2r;
  QUSB_U32 changed;           /* 1 = SCTLR, 2 = CBAR, 4 = CBA2R, 8 = S2CR */
  QUSB_U32 readback;          /* S2CR read after the commit (diagnostic only) */
} QUSB_BYPASS_LEASE;

/* Read-only planning: geometry, our stream slot and the bank to use. The DMA
 * range is only checked against the SMMU's declared address widths. */
QUSB_RESULT QcomUsbBypassPlan(QUSB_READ32 read32, void *cookie, QUSB_U64 dma,
                              QUSB_U32 bytes, QUSB_BYPASS_LEASE *lease);
QUSB_RESULT QcomUsbBypassEnter(QUSB_READ32 read32, QUSB_WRITE32 write32,
                               QUSB_BARRIER barrier, void *cookie,
                               QUSB_BYPASS_LEASE *lease);
QUSB_RESULT QcomUsbBypassRestore(QUSB_READ32 read32, QUSB_WRITE32 write32,
                                 QUSB_BARRIER barrier, void *cookie,
                                 QUSB_BYPASS_LEASE *lease);
/* Callback policy: only the planned register/value pairs are writable. */
int QcomUsbBypassWriteAllowed(const QUSB_BYPASS_LEASE *lease,
                              QUSB_U64 address, QUSB_U32 value);

#endif
