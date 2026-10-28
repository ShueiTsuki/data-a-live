/* SPDX-License-Identifier: MIT
 * Read-only qualification and an optional exclusive USB stream DMA lease.
 */
#ifndef QCOM_USB_QUALIFY_H
#define QCOM_USB_QUALIFY_H

typedef unsigned int QUSB_U32;
typedef unsigned long long QUSB_U64;
typedef int (*QUSB_READ32)(void *cookie, QUSB_U64 address, QUSB_U32 *value);
typedef int (*QUSB_WRITE32)(void *cookie, QUSB_U64 address, QUSB_U32 value);
typedef void (*QUSB_BARRIER)(void *cookie); /* Must issue DSB SY on AArch64. */
typedef void (*QUSB_DELAY_US)(void *cookie, QUSB_U32 microseconds);

#include "../include/QcomUsbSoc.h"
#define QUSB_GCC_BASE QUD_GCC_BASE
#define QUSB_DWC3_BASE QUD_DWC3_BASE
#define QUSB_SMMU_BASE QUD_SMMU_BASE
#define QUSB_SMMU_BYTES QUD_SMMU_BYTES
#define QUSB_STREAM_ID QUD_STREAM_ID

typedef enum {
  QusbQualified = 0,
  QusbBadArgument,
  QusbReadFailed,
  QusbClockOff,
  QusbResetAsserted,
  QusbUnknownDwc3,
  QusbUnknownSmmu,
  QusbGlobalDisabled,
  QusbExtendedStreamIds,
  QusbNoExplicitStream,
  QusbMultipleStreams,
  QusbSharedStream,
  QusbStreamTranslated,
  QusbStreamFaulting,
  QusbUnsupportedStreamType,
  QusbPowerDomainOff,
  QusbCoreRunning,
  QusbWriteFailed,
  QusbOwnershipChanged,
  QusbWrongCoreRole,
  QusbUnsafePowerState,
  QusbParentClockNotReady,
  QusbClockForcedOff,
  QusbClockTimeout,
  QusbNoFreeStream
} QUSB_RESULT;

typedef enum {
  QusbDmaUnqualified = 0,
  QusbDmaInheritedGlobalBypass,
  QusbDmaExactStreamBypass,
  QusbDmaInheritedUnmatchedBypass,
  QusbDmaInheritedSharedStreamBypass
} QUSB_DMA_MODE;

typedef struct {
  QUSB_U32 gcc_cbcr[5]; /* core, iface, aggregate, UTMI, sleep */
  QUSB_U32 gcc_bcr, gcc_gdscr, gcc_cfg_gdscr;
  QUSB_U32 gsnpsid, gctl, gusb2phycfg, dcfg, dctl, dsts;
} QUSB_INHERITED;

typedef struct {
  QUSB_DMA_MODE mode;
  QUSB_U32 id0, id1, id7, scr0;
  QUSB_U32 stream_count, match_count, index, smr, s2cr;
  QUSB_U32 extended_stream_ids, stream_id, stream_mask;
  QUSB_U32 context_bank, context_sctlr, context_cbar, context_tcr;
  QUSB_U64 context_ttbr0;
} QUSB_SMMU_STATE;

typedef struct {
  QUSB_U32 active, index, smr, original_s2cr, bypass_s2cr;
  QUSB_U32 extended_stream_ids, original_cr0;
  QUSB_U32 new_entry, original_smr, stream_count, phase;
  /* Last value read back from the pair slot during this lease; 0xffffffff
   * means the write callback itself failed (no hardware access happened). */
  QUSB_U32 readback;
} QUSB_DMA_LEASE;

enum {
  QusbLeasePrepareS2cr = 1,
  QusbLeasePrepareSmr = 2,
  QusbLeaseCommit = 3,
  QusbLeaseComplete = 4,
  QusbLeaseInvalidate = 5,
  QusbLeaseRestoreFirst = 6,
  QusbLeaseRestoreLast = 7,
  QusbLeaseRestored = 8,
  QusbLeaseExistingCommit = 9,
  QusbLeaseExistingRestore = 10
};

/* Pure callback policy: only the selected slot and the exact value for its
 * current phase are writable. The helper separately verifies live ownership.
 */
int QcomUsbDmaWriteAllowed(const QUSB_DMA_LEASE *lease,
                           QUSB_U64 address, QUSB_U32 value);

typedef struct {
  QUSB_U32 active_mask, original[5];
  QUSB_U32 gdscr, cfg_gdscr, bcr, master_cmd, master_cfg, utmi_cmd, utmi_cfg, pll0_mode;
  QUSB_U32 failed_index;
} QUSB_CLOCK_LEASE;

/* Optional branch-only resume. CFG_GDSCR.POWER_UP_COMPLETE must already be on,
 * with SW_COLLAPSE/HW_CONTROL clear. No power-domain, parent RCG, PLL, reset,
 * PMIC or rail writes. Only ENABLE bit0 of the five USB CBCRs can be changed.
 * Caller must own USB, prevent competing clock/firmware callbacks, retain ABL
 * DMA buffers until it can inspect/halt the core, and bound poll_us to 1..1000.
 * A failure may leave active_mask set and needs RestoreClocks after all USB
 * DMA is stopped and the SMMU lease has been restored.
 */
QUSB_RESULT QcomUsbResumeClocks(QUSB_READ32 read32, QUSB_WRITE32 write32,
                               QUSB_BARRIER barrier, QUSB_DELAY_US delay_us,
                               void *cookie, QUSB_U32 poll_us,
                               QUSB_CLOCK_LEASE *lease);
QUSB_RESULT QcomUsbRestoreClocks(QUSB_READ32 read32, QUSB_WRITE32 write32,
                                QUSB_BARRIER barrier, void *cookie,
                                QUSB_CLOCK_LEASE *lease);

/* GCC must be mapped as device memory. No clock/reset/PHY changes are made.
 * Checks the five live-DT named clocks before reading DWC3. A successful result
 * shows register accessibility; it does not prove the eUSB2 PHY is trained.
 */
QUSB_RESULT QcomUsbInspectInherited(QUSB_READ32 read32, void *cookie,
                                   QUSB_INHERITED *state);

/* The live-validated qcom,qsmmu-v500 SMMUv2 layout, non-secure register-view
 * and non-secure USB DMA are platform preconditions.
 * Only QusbQualified authorizes physical-address DMA: an existing CLIENTPD=1
 * globally bypasses translation, an unmatched stream bypasses by USFCFG=0,
 * or one valid matching SMR selects BYPASS (shared masks are read-only).
 * mode distinguishes these cases. Inherited global bypass requires no SMR
 * reads or writes. Translated state is reported, never modified by this
 * function, and must not be treated as an identity mapping.
 */
QUSB_RESULT QcomUsbQualifyIdentityDma(QUSB_READ32 read32, void *cookie,
                                    QUSB_SMMU_STATE *state);

/* OPTIONAL mutation, separate from read-only inspection. Requires a known
 * DWC3 in DEVICE role, RUN_STOP clear and DEVCTRLHLT set. A unique exact
 * translated SID0x40 can acquire a TYPE-only bypass lease. If the complete
 * table has no SID0x40 match and USFCFG=1, one invalid slot can instead be
 * leased for exact SID0x40: configure invalid S2CR/SMR, commit validity last.
 * Only that chosen pair is touched; no context/page-table, global control,
 * TLB, clock, power or other stream writes occur.
 * Caller must stop all USB DMA, own the USB core exclusively and keep the
 * lease until QcomUsbRestoreDma completes. A nonzero lease.active after any
 * error requires restoration before another owner touches USB.
 */
QUSB_RESULT QcomUsbEnterIdentityDma(QUSB_READ32 read32, QUSB_WRITE32 write32,
                                   QUSB_BARRIER barrier, void *cookie,
                                   QUSB_DMA_LEASE *lease,
                                   QUSB_SMMU_STATE *state);
QUSB_RESULT QcomUsbRestoreDma(QUSB_READ32 read32, QUSB_WRITE32 write32,
                             QUSB_BARRIER barrier, void *cookie,
                             QUSB_DMA_LEASE *lease);
const char *QcomUsbResultName(QUSB_RESULT result);

#endif
