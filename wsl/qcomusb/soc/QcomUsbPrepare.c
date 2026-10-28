/* SPDX-License-Identifier: MIT
 * SM8150/nabu device-mode bring-up for the USB log module.
 *
 * The log module inherits a ready PHY and a device-mode core. ABL hands the
 * controller over in host mode and halted, so this file does the missing half:
 * GDSC power, branch clocks, reset release, the femto HS PHY sequence and the
 * core's device-mode soft reset. Every wait is bounded and every read can come
 * back as zero when the caller's own deadline has expired.
 *
 * Order matters and follows the board's measured sequence (see the archived
 * NativeDwc3 driver): clocks before reset release, PHY before the core is put
 * into device mode, and VBUSVLDEXT0 must stay set across the D+ pulse.
 */
#include <stddef.h>
#include "../include/QcomUsbPrepare.h"

/* DWC3 core registers (offsets from the core base). */
#define DWC3_GSNPSID        0x0c120u
#define DWC3_GCTL           0x0c110u
#define DWC3_GSTS           0x0c118u
#define DWC3_GUSB2PHYCFG0   0x0c200u
#define DWC3_GUSB3PIPECTL0  0x0c2c0u
#define DWC3_DCFG           0x0c700u
#define DWC3_DCTL           0x0c704u
#define DWC3_DSTS           0x0c70cu

#define GCTL_CORESOFTRESET  (1u << 11)
#define GCTL_PRTCAP_DEVICE  (2u << 12)
#define GCTL_DSBLCLKGTNG    (1u << 0)
#define GSTS_CURMOD         0x3u
#define GSTS_CURMOD_DEVICE  0u   /* 0 = device, 1 = host, 2 = DRD */
#define GUSB2PHYCFG_PHYSOFTRST (1u << 31)
#define GUSB2PHYCFG_SUSPHY  (1u << 6)
#define GUSB2PHYCFG_ENBLSLPM (1u << 8)
#define GUSB3PIPECTL_SUSPHY (1u << 17)
#define DCTL_CSFTRST        (1u << 30)
#define DCTL_RUN_STOP       (1u << 31)
#define DSTS_DEVCTRLHLT     (1u << 22)
#define DCFG_DEVSPD_HS      0u   /* DCFG.DEVSPD: 0 = high speed (1 is full speed!) */

/* dwc3_set_mode() settles for 100 ms after the mode write; the module's
 * delay_us() refuses anything above 10 ms, so the wait is split. */
#define CORE_MODE_SETTLE_STEP_US 10000u
#define CORE_MODE_SETTLE_STEPS   10u

/* GCC bits. */
#define GDSCR_SW_COLLAPSE   (1u << 0)
#define GDSCR_HW_CONTROL    (1u << 1)
#define GDSCR_PWR_ON        (1u << 31)
#define CBCR_CLK_OFF        (1u << 31)
#define CBCR_ENABLE         (1u << 0)

/* femto HS PHY (phy-msm-snps-hs), offsets inside the QUD_PHY window. */
#define PHY_UTMI_CTRL0      0x3cu
#define PHY_UTMI_CTRL1      0x40u
#define PHY_UTMI_CTRL5      0x50u
#define PHY_COMMON0         0x54u
#define PHY_COMMON1         0x58u
#define PHY_COMMON2         0x5cu
#define PHY_CTRL1           0x60u
#define PHY_CTRL2           0x64u
#define PHY_OVERRIDE_X1     0x70u
#define PHY_OVERRIDE_X2     0x74u
#define PHY_CFG0            0x94u
#define PHY_REFCLK_CTRL     0xa0u

struct prep {
    const QCOM_PREPARE_PLATFORM *p;
    uint32_t *last;
};

static int
prep_read(struct prep *c, uint64_t address, uint32_t *value)
{
    if (!c->p->read32(c->p->cookie, address, value)) {
        return 0;
    }
    if (c->last) {
        *c->last = *value;
    }
    return 1;
}

static int
prep_write(struct prep *c, uint64_t address, uint32_t value)
{
    if (!c->p->write32(c->p->cookie, address, value)) {
        return 0;
    }
    if (c->last) {
        *c->last = value;
    }
    return 1;
}

static int
prep_mask(struct prep *c, uint64_t address, uint32_t mask, uint32_t set)
{
    uint32_t value;

    if (!prep_read(c, address, &value)) {
        return 0;
    }
    value = (value & ~mask) | (set & mask);
    return prep_write(c, address, value);
}

static void
prep_delay(struct prep *c, uint32_t usec)
{
    if (c->p->delay_us) {
        c->p->delay_us(c->p->cookie, usec);
    }
}

/* GDSC: clear SW_COLLAPSE/HW_CONTROL, then wait for PWR_ON. */
static QCOM_PREPARE_RESULT
prep_power(struct prep *c)
{
    uint32_t value;
    unsigned int i;

    if (!prep_read(c, QUD_GCC_BASE + QUD_GCC_GDSCR_OFFSET, &value)) {
        return QPREP_DEADLINE;
    }
    if (!prep_write(c, QUD_GCC_BASE + QUD_GCC_GDSCR_OFFSET,
                    value & ~(GDSCR_SW_COLLAPSE | GDSCR_HW_CONTROL))) {
        return QPREP_DEADLINE;
    }
    for (i = 0; i < 200u; ++i) {
        if (!prep_read(c, QUD_GCC_BASE + QUD_GCC_GDSCR_OFFSET, &value)) {
            return QPREP_DEADLINE;
        }
        if (value & GDSCR_PWR_ON) {
            return QPREP_OK;
        }
        prep_delay(c, 100);
    }
    return QPREP_GDSC;
}

/* Every USB branch clock: enable, then wait for CLK_OFF to drop. */
static QCOM_PREPARE_RESULT
prep_clocks(struct prep *c)
{
    static const uint32_t cbcr[] = QUD_GCC_CLOCK_OFFSETS;
    unsigned int i;

    for (i = 0; i < sizeof(cbcr) / sizeof(cbcr[0]); ++i) {
        if (!prep_mask(c, QUD_GCC_BASE + cbcr[i], CBCR_ENABLE, CBCR_ENABLE)) {
            return QPREP_DEADLINE;
        }
    }
    for (i = 0; i < sizeof(cbcr) / sizeof(cbcr[0]); ++i) {
        static const uint32_t no_poll[] = QUD_GCC_CLOCK_NO_POLL_OFFSETS;
        unsigned int tries, skip = 0;
        uint32_t value = CBCR_CLK_OFF;

        for (tries = 0; tries < sizeof(no_poll) / sizeof(no_poll[0]); ++tries) {
            if (no_poll[tries] && no_poll[tries] == cbcr[i]) {
                skip = 1;
            }
        }
        if (skip) {
            continue;   /* branch reports no usable halt status */
        }
        for (tries = 0; tries < 200u; ++tries) {
            if (!prep_read(c, QUD_GCC_BASE + cbcr[i], &value)) {
                return QPREP_DEADLINE;
            }
            if (!(value & CBCR_CLK_OFF)) {
                break;
            }
            prep_delay(c, 100);
        }
        if (value & CBCR_CLK_OFF) {
            return QPREP_CLOCKS;
        }
    }
    return QPREP_OK;
}

/* The USB30_PRIM reset does not self-clear: assert, then release explicitly. */
static QCOM_PREPARE_RESULT
prep_reset(struct prep *c)
{
    uint32_t value;

    if (!prep_write(c, QUD_GCC_BASE + QUD_GCC_BCR_OFFSET, 1u)) {
        return QPREP_DEADLINE;
    }
    prep_delay(c, 1000);
    if (!prep_write(c, QUD_GCC_BASE + QUD_GCC_BCR_OFFSET, 0u)) {
        return QPREP_DEADLINE;
    }
    prep_delay(c, 1000);
    /* The PHY's own resets do not self-clear either. */
    if (QUD_GCC_AHB2PHY_BCR_OFFSET) {
        if (!prep_write(c, QUD_GCC_BASE + QUD_GCC_AHB2PHY_BCR_OFFSET, 1u)) return QPREP_DEADLINE;
        prep_delay(c, 1000);
        if (!prep_write(c, QUD_GCC_BASE + QUD_GCC_AHB2PHY_BCR_OFFSET, 0u)) return QPREP_DEADLINE;
    }
    if (QUD_GCC_QUSB2PHY_BCR_OFFSET) {
        if (!prep_write(c, QUD_GCC_BASE + QUD_GCC_QUSB2PHY_BCR_OFFSET, 1u)) return QPREP_DEADLINE;
        prep_delay(c, 1000);
        if (!prep_write(c, QUD_GCC_BASE + QUD_GCC_QUSB2PHY_BCR_OFFSET, 0u)) return QPREP_DEADLINE;
    }
    if (!prep_read(c, QUD_GCC_BASE + QUD_GCC_BCR_OFFSET, &value)) {
        return QPREP_DEADLINE;
    }
    return (value & 1u) ? QPREP_RESET : QPREP_OK;
}

static QCOM_PREPARE_RESULT
prep_identity(struct prep *c)
{
    uint32_t id;
    uint32_t family;

    if (!prep_read(c, QUD_DWC3_BASE + DWC3_GSNPSID, &id)) {
        return QPREP_DEADLINE;
    }
    family = id & 0xffff0000u;
    if (family != 0x55330000u && family != 0x33310000u) {
        return QPREP_ID;
    }
    return QPREP_OK;
}

/* femto HS PHY: msm_hsphy_init() order, then the D+ pulse with XCVRSEL at 0x40. */
static QCOM_PREPARE_RESULT
prep_phy(struct prep *c)
{
    uint32_t value;
    unsigned int i;

    if (!prep_mask(c, QUD_PHY_BASE + PHY_CFG0, 1u << 1, 1u << 1)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_UTMI_CTRL5, 1u << 1, 1u << 1)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_COMMON0, 0x70u, 0u)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_COMMON1, 1u << 5, 1u << 5)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_REFCLK_CTRL, 0x3u, 0x2u)) return QPREP_DEADLINE;
    /* Software VBUS: select the external source and drive it to 1. */
    if (!prep_mask(c, QUD_PHY_BASE + PHY_COMMON1, 1u << 4, 1u << 4)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_CTRL1, 1u << 0, 1u << 0)) return QPREP_DEADLINE;
    /* qcom,param-override-seq = <0x04 0x70 0x2c 0x74>: value first, then offset. */
    if (!prep_write(c, QUD_PHY_BASE + PHY_OVERRIDE_X1, 0x04u)) return QPREP_DEADLINE;
    if (!prep_write(c, QUD_PHY_BASE + PHY_OVERRIDE_X2, 0x2cu)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_COMMON2, 1u << 0, 1u << 0)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_CTRL2, (1u << 3) | (1u << 2), (1u << 3) | (1u << 2))) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_UTMI_CTRL0, 1u << 0, 1u << 0)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_UTMI_CTRL5, 1u << 1, 0u)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_CTRL2, 1u << 3, 0u)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_CFG0, 1u << 1, 0u)) return QPREP_DEADLINE;
    prep_delay(c, 5000);

    /* D+ pulse (msm_hsphy_drive_dp_pulse): XCVRSEL lives at 0x40; 0x60 bit0 is
     * VBUSVLDEXT0 and must not be touched here. */
    if (!prep_mask(c, QUD_PHY_BASE + PHY_CFG0, 0x3u, 0x3u)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_UTMI_CTRL0, 0x18u, 0x00u)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_UTMI_CTRL0, 1u << 5, 1u << 5)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_UTMI_CTRL1, 1u << 0, 1u << 0)) return QPREP_DEADLINE;
    prep_delay(c, 200000 / 1000);
    if (!prep_mask(c, QUD_PHY_BASE + PHY_UTMI_CTRL0, 1u << 5, 0u)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_UTMI_CTRL1, 1u << 0, 0u)) return QPREP_DEADLINE;
    if (!prep_mask(c, QUD_PHY_BASE + PHY_CFG0, 0x3u, 0u)) return QPREP_DEADLINE;

    /* VBUS select + override must both still read back as 1. */
    if (!prep_read(c, QUD_PHY_BASE + PHY_COMMON1, &value)) return QPREP_DEADLINE;
    if (!(value & (1u << 4))) return QPREP_PHY;
    if (!prep_read(c, QUD_PHY_BASE + PHY_CTRL1, &value)) return QPREP_DEADLINE;
    if (!(value & 1u)) return QPREP_PHY;
    /* The core's QSCRATCH VBUS bit is a second, independent assertion. */
    if (!prep_mask(c, QUD_DWC3_BASE + QUD_QSCRATCH_HS_PHY_CTRL_OFFSET,
                   QUD_QSCRATCH_VBUS_MASK, QUD_QSCRATCH_VBUS_MASK)) return QPREP_DEADLINE;
    (void)i;
    return QPREP_OK;
}

/* The core latches CORESOFTRESET: software has to clear it, exactly as Linux
 * dwc3_core_init() does. The board read GCTL back as 0x00102800 after a 20 ms
 * poll (2026-10-26), i.e. the bit never fell on its own. */
static QCOM_PREPARE_RESULT
prep_core_reset(struct prep *c)
{
    uint32_t value;
    unsigned int i;

    if (!prep_read(c, QUD_DWC3_BASE + DWC3_GCTL, &value)) return QPREP_DEADLINE;
    if (!prep_write(c, QUD_DWC3_BASE + DWC3_GCTL, value | GCTL_CORESOFTRESET)) return QPREP_DEADLINE;
    /* Observing the bit set is best effort: a fast core may finish first. */
    for (i = 0; i < 20u; ++i) {
        if (!prep_read(c, QUD_DWC3_BASE + DWC3_GCTL, &value)) return QPREP_DEADLINE;
        if (value & GCTL_CORESOFTRESET) break;
        prep_delay(c, 100);
    }
    if (!prep_write(c, QUD_DWC3_BASE + DWC3_GCTL,
                    value & ~GCTL_CORESOFTRESET)) return QPREP_DEADLINE;
    for (i = 0; i < 200u; ++i) {
        if (!prep_read(c, QUD_DWC3_BASE + DWC3_GCTL, &value)) return QPREP_DEADLINE;
        if (!(value & GCTL_CORESOFTRESET)) return QPREP_OK;
        prep_delay(c, 100);
    }
    return QPREP_CORE;
}

/* Core: soft reset, device mode, HS speed, RUN.
 *
 * Acceptance reads GSTS, not GCTL: GCTL is a control register and reads back
 * whatever was written, so it cannot prove the core switched. The reset is
 * cleared and verified first, otherwise a core still held in reset can report
 * CURMOD = 0 and pass by accident. */
static QCOM_PREPARE_RESULT
prep_core(struct prep *c)
{
    QCOM_PREPARE_RESULT result;
    uint32_t value;
    unsigned int i;

    result = prep_core_reset(c);
    if (result != QPREP_OK) return result;

    if (!prep_write(c, QUD_DWC3_BASE + DWC3_GCTL,
                    GCTL_PRTCAP_DEVICE | GCTL_DSBLCLKGTNG)) return QPREP_DEADLINE;
    for (i = 0; i < CORE_MODE_SETTLE_STEPS; ++i) {
        prep_delay(c, CORE_MODE_SETTLE_STEP_US);
    }
    if (!prep_read(c, QUD_DWC3_BASE + DWC3_GSTS, &value)) return QPREP_DEADLINE;
    if ((value & GSTS_CURMOD) != GSTS_CURMOD_DEVICE) return QPREP_CORE;

    /* High-speed-only port: a live SS pipe makes the core negotiate a
     * SuperSpeed link that has no lane, and the host sees nothing. */
    if (!prep_mask(c, QUD_DWC3_BASE + DWC3_GUSB3PIPECTL0,
                   GUSB3PIPECTL_SUSPHY, GUSB3PIPECTL_SUSPHY)) return QPREP_DEADLINE;

    /* PHY soft reset pulse with sleep mode and LPM suspend released. */
    if (!prep_mask(c, QUD_DWC3_BASE + DWC3_GUSB2PHYCFG0,
                   GUSB2PHYCFG_PHYSOFTRST | GUSB2PHYCFG_SUSPHY | GUSB2PHYCFG_ENBLSLPM,
                   GUSB2PHYCFG_PHYSOFTRST)) return QPREP_DEADLINE;
    prep_delay(c, 1000);
    if (!prep_mask(c, QUD_DWC3_BASE + DWC3_GUSB2PHYCFG0,
                   GUSB2PHYCFG_PHYSOFTRST, 0u)) return QPREP_DEADLINE;

    if (!prep_write(c, QUD_DWC3_BASE + DWC3_DCFG, DCFG_DEVSPD_HS)) return QPREP_DEADLINE;

    /* Same latched behaviour on the device-controller reset. */
    if (!prep_mask(c, QUD_DWC3_BASE + DWC3_DCTL, DCTL_CSFTRST, DCTL_CSFTRST)) return QPREP_DEADLINE;
    prep_delay(c, 1000);
    if (!prep_mask(c, QUD_DWC3_BASE + DWC3_DCTL, DCTL_CSFTRST, 0u)) return QPREP_DEADLINE;
    for (i = 0; i < 200u; ++i) {
        if (!prep_read(c, QUD_DWC3_BASE + DWC3_DCTL, &value)) return QPREP_DEADLINE;
        if (!(value & DCTL_CSFTRST)) break;
        prep_delay(c, 100);
    }
    if (value & DCTL_CSFTRST) return QPREP_CORE;

    if (!prep_mask(c, QUD_DWC3_BASE + DWC3_DCTL, DCTL_RUN_STOP, DCTL_RUN_STOP)) return QPREP_DEADLINE;

    /* The core must leave halt. Connect speed is recorded, not gated: the host
     * has not reset the port yet, so CONNECTSPD still holds the previous
     * connection's value and gating on it would fail a healthy core. */
    for (i = 0; i < 200u; ++i) {
        if (!prep_read(c, QUD_DWC3_BASE + DWC3_DSTS, &value)) return QPREP_DEADLINE;
        if (!(value & DSTS_DEVCTRLHLT)) return QPREP_OK;
        prep_delay(c, 100);
    }
    return QPREP_CORE;
}

QCOM_PREPARE_RESULT
QcomUsbPrepare(const QCOM_PREPARE_PLATFORM *platform, uint32_t *failed_stage,
               uint32_t *last_readback)
{
    struct prep c;
    QCOM_PREPARE_RESULT result;

    if (failed_stage) *failed_stage = QPREP_STAGE_START;
    if (last_readback) *last_readback = 0;
    if (!platform || !platform->read32 || !platform->write32 || !QUD_PHY_BASE) {
        return QPREP_ARG;
    }
    c.p = platform;
    c.last = last_readback;

    if (failed_stage) *failed_stage = QPREP_STAGE_CLOCKS;
    result = prep_power(&c);
    if (result == QPREP_OK) result = prep_clocks(&c);
    if (result == QPREP_OK) result = prep_reset(&c);
    if (result != QPREP_OK) return result;

    if (failed_stage) *failed_stage = QPREP_STAGE_ID;
    result = prep_identity(&c);
    if (result != QPREP_OK) return result;

    if (failed_stage) *failed_stage = QPREP_STAGE_PHY;
    result = prep_phy(&c);
    if (result != QPREP_OK) return result;

    if (failed_stage) *failed_stage = QPREP_STAGE_CORE;
    return prep_core(&c);
}
