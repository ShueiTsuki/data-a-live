/* SPDX-License-Identifier: MIT
 * SM8850 (Xiaomi 17 Pro, pandora) GCC + APPS SMMU + DWC3 profile for Data A Live.
 *
 * Sources:
 *   DT     device/pandora-live.dts: ssusb@a600000 (qcom,dwc-usb3-msm; dwc3@a600000 iommus
 *          <&apps_smmu 0x40 0>; five clocks core/iface/bus_aggr/utmi/sleep, reset, power domain),
 *          apps-smmu@15000000 (qcom,qsmmu-v500), clock-controller@100000 (qcom,canoe-gcc)
 *   GCC    mainline drivers/clk/qcom/gcc-kaanapali.c (offsets by clock name; the DT's clock ids
 *          are Android's own numbering)
 *   board  device/logs/usb1.txt (k0248, pandora_usb=1, after `fastboot boot`): all five CBCRs
 *          0x08000001 (on), BCR 0, GDSCR 0xfa22f800 (PWR_ON, SW_COLLAPSE and HW_CONTROL clear),
 *          CFG_GDSCR 0x04070000 (POWER_UP_COMPLETE); DWC3 GSNPSID 0x33313130, GCTL 0x00102005
 *          (device role), DCTL RUN_STOP clear, DSTS DEVCTRLHLT set
 *   SMMU   device/logs/spk6.txt (the audio survey): stream 0x40 is group 3 -> bank 3, S2CR
 *          type translate, bank 3 SCTLR 0x000000e0 (M clear), CBAR 0x0001f000
 *          (S1_TRANS_S2_BYPASS), CBA2R 1; a TYPE=BYPASS S2CR reads back as FAULT here too
 */
#ifndef QUD_SM8850_H
#define QUD_SM8850_H

#define QUD_SOC_NAME "sm8850-pandora"

/* Development IDs inherited from m1n1 through the module, not a product allocation. */
#define QUD_USB_VID 0x1209u
#define QUD_USB_PID 0x316du
#define QUD_USB_SERIAL "QcomXnuUsbDebug"
#define QUD_USB_PRODUCT "Qualcomm XNU Debug"

#define QUD_GCC_BASE 0x00100000ULL
#define QUD_GCC_BYTES 0x40000u
#define QUD_DWC3_BASE 0x0a600000ULL           /* DT core_base <0xa600000 0x100000> */
#define QUD_DWC3_BYTES 0x100000u
#define QUD_SMMU_BASE 0x15000000ULL
#define QUD_SMMU_BYTES 0x100000ULL
#define QUD_STREAM_ID 0x40u

/* gcc-kaanapali.c clk_branch halt_reg of the DT's clocks, in the module's order core, iface,
 * aggregate, UTMI, sleep: usb30_prim_master, cfg_noc_usb3_prim_axi, aggre_usb3_prim_axi,
 * usb30_prim_mock_utmi, usb30_prim_sleep. */
#define QUD_GCC_CBCR_OFFSETS {0x39018u, 0x39090u, 0x39094u, 0x3902cu, 0x39028u}
#define QUD_GCC_BCR_OFFSET 0x39000u           /* GCC_USB30_PRIM_BCR */
#define QUD_GCC_GDSCR_OFFSET 0x39004u         /* usb30_prim_gdsc */
#define QUD_GCC_CFG_GDSCR_OFFSET 0x39008u     /* gdscr + 4: POWER_UP_COMPLETE bit 16 */

/* dwc3-msm QSCRATCH at 0xf8800: HS_PHY_CTRL's software VBUS valid (20) and session valid
 * select (28), as on SM8550. */
#define QUD_QSCRATCH_HS_PHY_CTRL_OFFSET 0xf8810u
#define QUD_QSCRATCH_VBUS_MASK ((1u << 20) | (1u << 28))

/* The bootloader already routes stream 0x40 to bank 3 with translation off, and this firmware
 * turns a TYPE=BYPASS S2CR into FAULT: take the bypass-bank route (soc/QcomUsbBypass.c), DMA at
 * physical addresses. The private-domain IOVA below is then unused but must stay valid. */
#define QUD_DMA_PREFER_IDENTITY 1
#define QUD_DMA_BYPASS_BANK 1
#define QUD_DMA_IOVA 0x90000000ULL            /* also the start of the DT's dwc3_mem_region window */

/* Started from a running kernel by bsd/dev/pandora_usb.c, whose thread polls the controller:
 * no interrupts-masked wait for enumeration inside qcom_usb_debug_init. */
#define QUD_INIT_ENUM_WAIT_MS 0u

/* PE_halt_restart belongs to bsd/dev/sweet_ufs.c (log flush, then PSCI reset): no module hook. */
#define QUD_PSCI_SMC_RESET 0

/* No prepare stage: the inherited state above already qualifies; QUD_PHY_BASE stays 0. */

#endif
