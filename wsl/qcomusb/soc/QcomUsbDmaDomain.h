/* SPDX-License-Identifier: MIT */
#ifndef SOCRATES_QCOM_USB_DMA_DOMAIN_H
#define SOCRATES_QCOM_USB_DMA_DOMAIN_H
#include "QcomUsbQualify.h"

#define QUSB_DOMAIN_IOVA QUD_DMA_IOVA
#define QUSB_DOMAIN_DMA_BYTES 32768U
#define QUSB_DOMAIN_PT_BYTES 12288U
#define QUSB_DOMAIN_PAGE_BYTES 4096U
#define QUSB_DOMAIN_REGISTER_COUNT 10U

typedef int (*QUSB_READ64)(void *, QUSB_U64, QUSB_U64 *);
typedef int (*QUSB_WRITE64)(void *, QUSB_U64, QUSB_U64);
typedef int (*QUSB_CLEAN_TABLES)(void *, void *cpu, QUSB_U32 bytes);
typedef struct {
  void *cookie;
  QUSB_READ32 read32;
  QUSB_WRITE32 write32;
  QUSB_READ64 read64;
  QUSB_WRITE64 write64;
  QUSB_BARRIER barrier;
  QUSB_DELAY_US delay_us;
  QUSB_CLEAN_TABLES clean_tables;
} QUSB_DOMAIN_PLATFORM;

typedef enum {
  QusbDomainOkay = 0,
  QusbDomainBadArgument,
  QusbDomainReadFailed,
  QusbDomainUnsupported,
  QusbDomainCoreRunning,
  QusbDomainStreamExists,
  QusbDomainNoSlot,
  QusbDomainNoBank,
  QusbDomainNoAsid,
  QusbDomainOwnershipChanged,
  QusbDomainWriteFailed,
  QusbDomainTlbTimeout,
  QusbDomainCleanFailed
} QUSB_DOMAIN_RESULT;

typedef struct {
  QUSB_U64 address, saved, installed;
  QUSB_U32 width, touched;
} QUSB_DOMAIN_REGISTER;

typedef struct {
  QUSB_U32 magic, active, phase, operation;
  QUSB_U32 index, cb, asid, extended_stream_ids, stream_count, bank_count;
  QUSB_U32 original_cr0, id0, id1, id2, old_smr, old_s2cr;
  QUSB_U32 new_smr, new_s2cr, old_actlr, stage;
  QUSB_U32 enable_attempted, tlb_released, tlb_poisoned;
  QUSB_U32 tcr2_preserved, tcr2_observed, tcr2_bound, tcr2_view_owned;
  QUSB_U32 tcr2_format_cba2r, tcr2_format_cbar, tcr2_capture_pending;
  QUSB_U64 cb_base, gr1_base, table_pa, dma_pa;
  QUSB_U64 iova, last_address, last_expected, last_readback;
  QUSB_DOMAIN_REGISTER regs[QUSB_DOMAIN_REGISTER_COUNT];
} QUSB_DOMAIN_LEASE;

/* Caller provides separately owned contiguous 3x4K page-table pages and an
 * owned 32KiB DMA arena. CPU table pointer can differ from its physical PA.
 * Install maps only the eight DMA pages at fixed IOVA0x90000000 using enabled
 * AArch64 S1 translation. No identity/BYPASS fallback exists.
 * No device/MMIO access in the page-table-only builder below.
 */
QUSB_DOMAIN_RESULT QcomUsbDomainBuildTables(void *table_cpu, QUSB_U64 table_pa,
                                           QUSB_U32 table_bytes, QUSB_U64 dma_pa,
                                           QUSB_U32 dma_bytes, QUSB_U32 pa_bits);
QUSB_DOMAIN_RESULT QcomUsbDomainInstall(const QUSB_DOMAIN_PLATFORM *platform,
                                       void *table_cpu, QUSB_U64 table_pa,
                                       QUSB_U32 table_bytes, QUSB_U64 dma_pa,
                                       QUSB_U32 dma_bytes, QUSB_DOMAIN_LEASE *lease);
QUSB_DOMAIN_RESULT QcomUsbDomainRestore(const QUSB_DOMAIN_PLATFORM *platform,
                                       QUSB_DOMAIN_LEASE *lease);
/* Pure phase/address/value policy for the platform's write callbacks.
 * width must be4 or8. Only selected CB configuration/localTLB and selected
 * exactSID40 SMR/S2CR can be written. Platform still checks MMIO mappings.
 */
int QcomUsbDomainWriteAllowed(const QUSB_DOMAIN_LEASE *lease,
                              QUSB_U64 address, QUSB_U64 value, QUSB_U32 width);
const char *QcomUsbDomainResultName(QUSB_DOMAIN_RESULT result);
#endif
