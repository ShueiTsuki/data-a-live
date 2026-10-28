/* SPDX-License-Identifier: MIT */
#ifndef QCOM_USB_SOC_H
#define QCOM_USB_SOC_H
/* Pass -DQUD_SOC_HEADER='"soc/MySoc.h"', with the module root on -I.
 * No fallback guesses: a build must explicitly select its hardware profile.
 * The supplied backend requires clk_branch2/CFG_GDSCR, Qualcomm QSCRATCH,
 * non-secure QSMMUv500 SMMUv2 and inherited DEVICE-mode PHY/power.
 */
#ifndef QUD_SOC_HEADER
#define QUD_SOC_HEADER "../soc/Sm8850.h"
#endif
#ifndef QUD_SOC_HEADER
#error "Select hardware with -DQUD_SOC_HEADER=\"soc/Sm8550.h\""
#endif
#include QUD_SOC_HEADER
/* SM8550 (and other live GDSCs with qcom,support-cfg-gdscr) use
 * CFG_GDSCR.POWER_UP_COMPLETE bit16; older blocks without that property
 * expose the state in GDSCR.PWR_ON bit31 instead. */
/* Optional HS-PHY window for the device-mode prepare step; a profile that
 * does not define it cannot run QcomUsbPrepare (it returns QPREP_ARG). */
#ifndef QUD_GCC_CLOCK_OFFSETS
#define QUD_GCC_CLOCK_OFFSETS QUD_GCC_CBCR_OFFSETS
#define QUD_GCC_CLOCK_NO_POLL_OFFSETS {0u}
#define QUD_GCC_QUSB2PHY_BCR_OFFSET 0u
#define QUD_GCC_AHB2PHY_BCR_OFFSET 0u
#endif

#ifndef QUD_PHY_BASE
#define QUD_PHY_BASE 0ULL
#endif
#ifndef QUD_PHY_BYTES
#define QUD_PHY_BYTES 0u
#endif

#ifndef QUD_GDSC_LEGACY_PWR_ON
#define QUD_GDSC_LEGACY_PWR_ON 0
#endif
_Static_assert(QUD_GDSC_LEGACY_PWR_ON == 0 || QUD_GDSC_LEGACY_PWR_ON == 1,
               "GDSC power-state source must be 0 (CFG_GDSCR) or 1 (GDSCR)");
/* Boards whose bootloader already routes the USB SID through the SMMU cannot
 * install a private translated domain: the stream slot is taken. Selecting
 * this uses the module's audited identity lease instead (inherited bypass is
 * reused, a translated entry becomes TYPE-only bypass with the original pair
 * saved, and DMA uses physical addresses). */
#ifndef QUD_DMA_PREFER_IDENTITY
#define QUD_DMA_PREFER_IDENTITY 0
#endif
_Static_assert(QUD_DMA_PREFER_IDENTITY == 0 || QUD_DMA_PREFER_IDENTITY == 1,
               "identity DMA preference must be 0 or 1");
/* Firmware whose SMMU rejects a TYPE=BYPASS S2CR (it reads back as FAULT, as on
 * SM8150) needs the bypass context-bank route instead: SCTLR=0, CBAR
 * S1_TRANS_S2_BYPASS, CBA2R.VA64=1, and an ordinary TYPE=TRANSLATE S2CR that
 * holds the bank number. Selecting this uses soc/QcomUsbBypass.c. */
/* Data A Live: how long qcom_usb_debug_init waits, interrupts masked, for the
 * host to enumerate (the original 5 s); 0 leaves it to the caller's polling. */
#ifndef QUD_INIT_ENUM_WAIT_MS
#define QUD_INIT_ENUM_WAIT_MS 5000u
#endif
#ifndef QUD_DMA_BYPASS_BANK
#define QUD_DMA_BYPASS_BANK 0
#endif
_Static_assert(QUD_DMA_BYPASS_BANK == 0 || QUD_DMA_BYPASS_BANK == 1,
               "bypass-bank selection must be 0 or 1");
_Static_assert(QUD_PSCI_SMC_RESET == 0 || QUD_PSCI_SMC_RESET == 1,
               "declare whether the board qualifies PSCI reset via SMC");
_Static_assert(QUD_USB_VID > 0 && QUD_USB_VID <= 65535 &&
               QUD_USB_PID > 0 && QUD_USB_PID <= 65535, "USB IDs must fit 16 bits");
_Static_assert(QUD_STREAM_ID <= 0x7fffu, "legacy/extended SID must fit 15 bits");
_Static_assert(QUD_DMA_IOVA > 0 && QUD_DMA_IOVA <= 0xffe00000ULL &&
               (QUD_DMA_IOVA & 0x1fffffULL) == 0,
               "private three-page table builder needs a 2MiB-aligned 32-bit IOVA");
_Static_assert(QUD_GCC_BASE != 0 && QUD_DWC3_BASE != 0 && QUD_SMMU_BASE != 0 &&
               ((QUD_GCC_BASE | QUD_DWC3_BASE | QUD_SMMU_BASE) & 4095u) == 0,
               "MMIO bases must be nonzero and page aligned");
_Static_assert(QUD_GCC_BYTES >= 4 && QUD_DWC3_BYTES >= 0xc800u &&
               QUD_SMMU_BYTES >= 0x10000u, "MMIO windows too small");
_Static_assert(QUD_GCC_BASE + QUD_GCC_BYTES > QUD_GCC_BASE &&
               QUD_DWC3_BASE + QUD_DWC3_BYTES > QUD_DWC3_BASE &&
               QUD_SMMU_BASE + QUD_SMMU_BYTES > QUD_SMMU_BASE,
               "MMIO window overflow");
#define QUD_DISJOINT(a, n, b, m) ((a) + (n) <= (b) || (b) + (m) <= (a))
_Static_assert(QUD_DISJOINT(QUD_GCC_BASE, QUD_GCC_BYTES, QUD_DWC3_BASE, QUD_DWC3_BYTES) &&
               QUD_DISJOINT(QUD_GCC_BASE, QUD_GCC_BYTES, QUD_SMMU_BASE, QUD_SMMU_BYTES) &&
               QUD_DISJOINT(QUD_DWC3_BASE, QUD_DWC3_BYTES, QUD_SMMU_BASE, QUD_SMMU_BYTES),
               "MMIO windows overlap");
#define QUD_OFFSET_OK(o, n) (((o) & 3u) == 0 && (o) <= (n) - 4)
_Static_assert(QUD_OFFSET_OK(QUD_GCC_BCR_OFFSET, QUD_GCC_BYTES) &&
               QUD_OFFSET_OK(QUD_GCC_GDSCR_OFFSET, QUD_GCC_BYTES) &&
               QUD_OFFSET_OK(QUD_GCC_CFG_GDSCR_OFFSET, QUD_GCC_BYTES) &&
               QUD_OFFSET_OK(QUD_QSCRATCH_HS_PHY_CTRL_OFFSET, QUD_DWC3_BYTES),
               "platform register outside MMIO window");
#endif
