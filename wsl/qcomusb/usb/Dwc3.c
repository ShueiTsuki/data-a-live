/* SPDX-License-Identifier: MIT
 * Derived from Asahi Linux m1n1 v1.4.17 usb_dwc3.c. See LICENSE.m1n1 and
 * ../docs/architecture.md. DART, allocation, Apple platform and the second CDC port removed.
 */
#include "Dwc3.h"
#include "Dwc3Regs.h"

#define EP_COUNT 6u
#define EP_NOTIFY 3u
#define EP_OUT 4u
#define EP_IN 5u
#define EVENT_BYTES 4096u
#define TRB_OFFSET 4096u
#define BUFFER_OFFSET 8192u
#define EP_BUFFER_BYTES 4096u
#define TX_BYTES DWC3_TX_BYTES
#define COMMAND_POLLS 10000u
#define CONFIG_ACTION_MODIFY (2u << 30)

enum control_state {
    CTRL_SETUP, CTRL_IN_WAIT, CTRL_IN_ACTIVE, CTRL_OUT_WAIT, CTRL_OUT_ACTIVE,
    CTRL_STATUS_IN_WAIT, CTRL_STATUS_IN_ACTIVE, CTRL_STATUS_OUT_WAIT,
    CTRL_STATUS_OUT_ACTIVE
};

struct dwc3 {
    struct dwc3_platform io;
    uint8_t *dma_cpu;
    uint64_t dma_device;
    uint32_t event_offset, tx_head, tx_tail, tx_used, in_length;
    uint32_t rx_head, rx_tail, rx_used, rx_generation;
    uint64_t rx_overflow;
    uint32_t packet_size;
    uint32_t resource[EP_COUNT];
    uint8_t active[EP_COUNT];
    uint8_t tx[TX_BYTES], rx[DWC3_RX_BYTES], line_coding[7], control_reply[256];
    uint16_t control_length;
    uint8_t control_state, control_three_stage, configured, dtr;
    uint8_t zlp_pending, running, initialized, full_speed, usb31;
    int failed;
    uint32_t events, commands, last_event;
    uint64_t dropped;
    uint8_t device_descriptor[18], manufacturer[64], product[64], serial[64];
};

_Static_assert(sizeof(struct dwc3) <= DWC3_CONTEXT_BYTES,
               "DWC3 context exceeds the public storage budget");

static const uint8_t device_descriptor[] = {
    18, 1, 0, 2, 2, 0, 0, 64, 0, 0, 0, 0,
    0, 1, 1, 2, 3, 1
};
static const uint8_t qualifier[] = {10, 6, 0, 2, 2, 0, 0, 64, 1, 0};
#define CONFIG_BYTES(mpslo, mpshi, dtype) \
    9, dtype, 67, 0, 2, 1, 0, 0x80, 50, \
    9, 4, 0, 0, 1, 2, 2, 1, 0, \
    5, 0x24, 0, 0x10, 1, \
    5, 0x24, 1, 0, 1, \
    4, 0x24, 2, 2, \
    5, 0x24, 6, 0, 1, \
    7, 5, 0x81, 3, 16, 0, 10, \
    9, 4, 1, 0, 2, 0x0a, 0, 0, 0, \
    7, 5, 0x02, 2, mpslo, mpshi, 0, \
    7, 5, 0x82, 2, mpslo, mpshi, 0
static const uint8_t config_hs[] = {CONFIG_BYTES(0, 2, 2)};
static const uint8_t config_fs[] = {CONFIG_BYTES(64, 0, 2)};
static const uint8_t other_hs[] = {CONFIG_BYTES(0, 2, 7)};
static const uint8_t other_fs[] = {CONFIG_BYTES(64, 0, 7)};
static const uint8_t language[] = {4, 3, 9, 4};
/* Per-instance fixed storage; identity does not retain caller pointers. */
static size_t identity_chars(const char *text)
{
    size_t n;
    if (!text) return 0;
    for (n = 0; n < 32u; ++n) {
        unsigned char c = (unsigned char)text[n];
        if (!c) return n;
        if (c < 32u || c > 126u) return 0;
    }
    return 0;
}
static void string_descriptor(uint8_t *out, const char *text, size_t chars)
{
    size_t i;
    out[0] = (uint8_t)(2 + 2 * chars); out[1] = 3;
    for (i = 0; i < chars; ++i) {
        out[2 + 2 * i] = (uint8_t)text[i]; out[3 + 2 * i] = 0;
    }
}

static void zero(void *p, size_t n)
{
    uint8_t *d = p;
    while (n--) *d++ = 0;
}
static void copy(void *to, const void *from, size_t n)
{
    uint8_t *d = to;
    const uint8_t *s = from;
    while (n--) *d++ = *s++;
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd(struct dwc3 *d, uint32_t r) { return d->io.read32(d->io.cookie, r); }
static void wr(struct dwc3 *d, uint32_t r, uint32_t v) { d->io.write32(d->io.cookie, r, v); }
static void trace(struct dwc3 *d, enum dwc3_trace e, uint32_t a, uint32_t b)
{
    if (d->io.trace) d->io.trace(d->io.cookie, e, a, b);
}
static void sync(struct dwc3 *d, void *p, size_t n, enum dwc3_sync_direction dir)
{
    d->io.dma_sync(d->io.cookie, p, n, dir);
    d->io.barrier(d->io.cookie);
}
static uint8_t *buffer(struct dwc3 *d, unsigned ep)
{
    return d->dma_cpu + BUFFER_OFFSET + ep * EP_BUFFER_BYTES;
}
static struct dwc3_trb *trb(struct dwc3 *d, unsigned ep)
{
    return (struct dwc3_trb *)(void *)(d->dma_cpu + TRB_OFFSET + ep * 64u);
}
static void rx_reset(struct dwc3 *d)
{
    zero(d->rx, sizeof(d->rx));
    d->rx_head = d->rx_tail = d->rx_used = 0;
    d->rx_generation++;
}
static void rx_push(struct dwc3 *d, const uint8_t *bytes, size_t count)
{
    size_t i, accepted = count;
    if (accepted > DWC3_RX_BYTES - d->rx_used)
        accepted = DWC3_RX_BYTES - d->rx_used;
    for (i = 0; i < accepted; i++) {
        d->rx[d->rx_head] = bytes[i];
        d->rx_head = (d->rx_head + 1u) % DWC3_RX_BYTES;
    }
    d->rx_used += (uint32_t)accepted;
    d->rx_overflow += count - accepted;
}
static int wait_mask(struct dwc3 *d, uint32_t reg, uint32_t mask, uint32_t want)
{
    unsigned i;
    for (i = 0; i < COMMAND_POLLS; i++) {
        if ((rd(d, reg) & mask) == want) return 0;
        d->io.delay_us(d->io.cookie, 1);
    }
    trace(d, DWC3_TRACE_TIMEOUT, reg, rd(d, reg));
    return -3;
}
static int ep_command(struct dwc3 *d, unsigned ep, uint32_t cmd,
                      uint32_t p0, uint32_t p1, uint32_t p2)
{
    uint32_t result;
    uint32_t phy = rd(d, DWC3_GUSB2PHYCFG(0));
    uint32_t suspend = DWC3_GUSB2PHYCFG_SUSPHY | (1u << 8); /* ENBLSLPM */
    int ret = 0;
    d->commands++;
    /* DWC_usb31 1.90a guide 3.2.2, also Linux dwc3_send_gadget_ep_cmd:
     * HS/FS endpoint commands require both PHY suspend controls disabled. */
    if (phy & suspend) wr(d, DWC3_GUSB2PHYCFG(0), phy & ~suspend);
    wr(d, DWC3_DEPCMDPAR0(ep), p0);
    wr(d, DWC3_DEPCMDPAR1(ep), p1);
    wr(d, DWC3_DEPCMDPAR2(ep), p2);
    wr(d, DWC3_DEPCMD(ep), cmd | DWC3_DEPCMD_CMDACT);
    /* USB31 cannot use CMDACT polling to guarantee ENDTRANSFER completion.
     * The documented no-IOC sequence waits 1ms before buffer reuse. */
    if (d->usb31 && (cmd & 15u) == DWC3_DEPCMD_ENDTRANSFER) {
        d->io.delay_us(d->io.cookie, 1000);
        goto out;
    }
    if (wait_mask(d, DWC3_DEPCMD(ep), DWC3_DEPCMD_CMDACT, 0)) { ret = -3; goto out; }
    result = rd(d, DWC3_DEPCMD(ep));
    /* Command status is a four-bit field, unlike the old m1n1 header. */
    if ((result >> 12) & 15u) {
        trace(d, DWC3_TRACE_COMMAND, (ep << 16) | cmd, result);
        ret = -4; goto out;
    }
    if ((cmd & 15u) == DWC3_DEPCMD_STARTTRANSFER)
        d->resource[ep] = DWC3_DEPCMD_GET_RSC_IDX(result);
out:
    if (phy & suspend) wr(d, DWC3_GUSB2PHYCFG(0), phy);
    return ret;
}
static int end_transfer(struct dwc3 *d, unsigned ep)
{
    if (!d->active[ep]) return 0;
    if (ep_command(d, ep, DWC3_DEPCMD_ENDTRANSFER | DWC3_DEPCMD_HIPRI_FORCERM |
                   DWC3_DEPCMD_PARAM(d->resource[ep]), 0, 0, 0)) return -4;
    d->active[ep] = 0;
    return 0;
}
static int end_noncontrol(struct dwc3 *d)
{
    unsigned ep;
    for (ep = EP_NOTIFY; ep < EP_COUNT; ep++) if (end_transfer(d, ep)) return -4;
    d->zlp_pending = 0;
    return 0;
}
static int configure_ep(struct dwc3 *d, unsigned ep, unsigned type,
                        unsigned packet, int modify)
{
    uint32_t p0 = DWC3_DEPCFG_EP_TYPE(type) | DWC3_DEPCFG_MAX_PACKET_SIZE(packet);
    uint32_t p1 = DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_NOT_READY_EN |
                  DWC3_DEPCFG_EP_NUMBER(ep);
    /* FIFO number is the USB IN endpoint number, not physical endpoint index. */
    if (ep & 1u) p0 |= DWC3_DEPCFG_FIFO_NUMBER(ep >> 1);
    if (type == DWC3_DEPCMD_TYPE_INTR) p1 |= DWC3_DEPCFG_BINTERVAL_M1(9);
    if (modify) p0 |= CONFIG_ACTION_MODIFY;
    if (ep_command(d, ep, DWC3_DEPCMD_SETEPCONFIG, p0, p1, 0)) return -4;
    if (!modify && ep_command(d, ep, DWC3_DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) return -4;
    return 0;
}
static int transfer(struct dwc3 *d, unsigned ep, uint32_t type, unsigned bytes)
{
    struct dwc3_trb *t = trb(d, ep);
    uint64_t data_dma = d->dma_device + BUFFER_OFFSET + ep * EP_BUFFER_BYTES;
    uint64_t trb_dma = d->dma_device + TRB_OFFSET + ep * 64u;
    if (d->active[ep] || bytes > EP_BUFFER_BYTES) return -5;
    t->bpl = (uint32_t)data_dma;
    t->bph = (uint32_t)(data_dma >> 32);
    t->size = bytes;
    t->ctrl = type | DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_LST |
              DWC3_TRB_CTRL_ISP_IMI | DWC3_TRB_CTRL_IOC;
    sync(d, buffer(d, ep), EP_BUFFER_BYTES, DWC3_TO_DEVICE);
    sync(d, t, 64, DWC3_TO_DEVICE);
    if (ep_command(d, ep, DWC3_DEPCMD_STARTTRANSFER,
                   (uint32_t)(trb_dma >> 32), (uint32_t)trb_dma, 0)) return -4;
    d->active[ep] = 1;
    return 0;
}
static int setup_phase(struct dwc3 *d)
{
    d->control_state = CTRL_SETUP;
    return transfer(d, 0, DWC3_TRBCTL_CONTROL_SETUP, 8);
}
static int stall(struct dwc3 *d)
{
    trace(d, DWC3_TRACE_UNSUPPORTED, buffer(d, 0)[0], buffer(d, 0)[1]);
    if (ep_command(d, 0, DWC3_DEPCMD_SETSTALL, 0, 0, 0)) return -4;
    return setup_phase(d);
}

const void *dwc3_descriptor(const struct dwc3 *d, unsigned type, unsigned index, int fs, size_t *length)
{
    const uint8_t *p = 0;
    size_t n = 0;
    if (!d || !d->initialized) { if (length) *length = 0; return 0; }
    if (index == 0) {
        switch (type) {
        case 1: p = d->device_descriptor; n = sizeof(d->device_descriptor); break;
        case 2: p = fs ? config_fs : config_hs; n = sizeof(config_hs); break;
        case 6: p = qualifier; n = sizeof(qualifier); break;
        case 7: p = fs ? other_hs : other_fs; n = sizeof(other_hs); break;
        }
    }
    if (type == 3) {
        switch (index) {
        case 0: p = language; n = sizeof(language); break;
        case 1: p = d->manufacturer; n = p[0]; break;
        case 2: p = d->product; n = p[0]; break;
        case 3: p = d->serial; n = p[0]; break;
        }
    }
    if (length) *length = n;
    return p;
}

static int set_configuration(struct dwc3 *d, unsigned value)
{
    if (value > 1) return -1;
    if (d->configured && end_noncontrol(d)) return -4;
    d->configured = (uint8_t)value;
    d->dtr = 0;
    wr(d, DWC3_DALEPENA, 3u | (value ? ((1u << EP_NOTIFY) | (1u << EP_OUT) |
                                       (1u << EP_IN)) : 0u));
    return 0;
}
static int setup_request(struct dwc3 *d)
{
    const uint8_t *s = buffer(d, 0), *reply = 0;
    uint16_t value = le16(s + 2), index = le16(s + 4), length = le16(s + 6);
    size_t n = 0;
    int ok = 0;
    trace(d, DWC3_TRACE_SETUP, ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) | value,
          ((uint32_t)index << 16) | length);
    d->control_three_stage = length != 0;
    d->control_length = 0;
    if (s[0] == 0x80 && s[1] == 6) {
        reply = dwc3_descriptor(d, value >> 8, value & 255, d->full_speed, &n);
        ok = reply != 0;
    } else if ((s[0] == 0x80 || s[0] == 0x81 || s[0] == 0x82) && s[1] == 0 &&
               value == 0 && length == 2 && ((s[0] == 0x80 && index == 0) ||
               (s[0] == 0x81 && index < 2) ||
               (s[0] == 0x82 && (index == 0 || index == 0x80 || index == 0x81 ||
                                  index == 2 || index == 0x82)))) {
        d->control_reply[0] = 0; d->control_reply[1] = 0;
        reply = d->control_reply; n = 2; ok = 1;
    } else if (s[0] == 0 && s[1] == 5 && value <= 127 && index == 0 && length == 0) {
        wr(d, DWC3_DCFG, (rd(d, DWC3_DCFG) & ~DWC3_DCFG_DEVADDR_MASK) |
                         DWC3_DCFG_DEVADDR(value));
        ok = 1;
    } else if (s[0] == 0 && s[1] == 9 && index == 0 && length == 0 && value <= 1) {
        int ret = set_configuration(d, value);
        if (ret) return ret;
        ok = 1;
    } else if (s[0] == 0x80 && s[1] == 8 && value == 0 && index == 0 && length == 1) {
        d->control_reply[0] = d->configured; reply = d->control_reply; n = 1; ok = 1;
    } else if (s[0] == 0x81 && s[1] == 10 && value == 0 && index < 2 && length == 1) {
        d->control_reply[0] = 0; reply = d->control_reply; n = 1; ok = 1;
    } else if (s[0] == 1 && s[1] == 11 && value == 0 && index < 2 && length == 0) {
        ok = 1;
    } else if (s[0] == 0x21 && s[1] == 0x22 && index == 0 && length == 0 && value <= 3) {
        d->dtr = (uint8_t)(value & 1);
        trace(d, DWC3_TRACE_READY, d->configured, d->dtr); ok = 1;
    } else if (s[0] == 0xa1 && s[1] == 0x21 && index == 0 && value == 0 && length == 7) {
        reply = d->line_coding; n = 7; ok = 1;
    } else if (s[0] == 0x21 && s[1] == 0x20 && index == 0 && value == 0 && length == 7) {
        d->control_length = 7; d->control_state = CTRL_OUT_WAIT; return 0;
    } else if (s[0] == 2 && s[1] == 1 && value == 0 && length == 0 &&
               (index == 0x81 || index == 2 || index == 0x82)) {
        unsigned ep = ((index & 15u) << 1) | ((index >> 7) & 1u);
        ok = ep_command(d, ep, DWC3_DEPCMD_CLEARSTALL, 0, 0, 0) == 0;
    }
    if (!ok) return stall(d);
    if (length) {
        if (!(s[0] & 0x80) || !reply) return stall(d);
        if (n > length) n = length;
        if (n > sizeof(d->control_reply)) return stall(d);
        copy(buffer(d, 1), reply, n);
        d->control_length = (uint16_t)n;
        d->control_state = CTRL_IN_WAIT;
    } else d->control_state = CTRL_STATUS_IN_WAIT;
    return 0;
}

static int control_not_ready(struct dwc3 *d, unsigned ep, unsigned status)
{
    uint32_t status_type = d->control_three_stage ? DWC3_TRBCTL_CONTROL_STATUS3 :
                                                  DWC3_TRBCTL_CONTROL_STATUS2;
    switch (d->control_state) {
    case CTRL_IN_WAIT:
        if (ep != 1 || status != DEPEVT_STATUS_CONTROL_DATA) return 0;
        d->control_state = CTRL_IN_ACTIVE;
        return transfer(d, 1, DWC3_TRBCTL_CONTROL_DATA, d->control_length);
    case CTRL_OUT_WAIT:
        if (ep != 0 || status != DEPEVT_STATUS_CONTROL_DATA) return 0;
        d->control_state = CTRL_OUT_ACTIVE;
        return transfer(d, 0, DWC3_TRBCTL_CONTROL_DATA, 64);
    case CTRL_STATUS_IN_WAIT:
        if (ep != 1 || status != DEPEVT_STATUS_CONTROL_STATUS) return 0;
        d->control_state = CTRL_STATUS_IN_ACTIVE;
        return transfer(d, 1, status_type, 0);
    case CTRL_STATUS_OUT_WAIT:
        if (ep != 0 || status != DEPEVT_STATUS_CONTROL_STATUS) return 0;
        d->control_state = CTRL_STATUS_OUT_ACTIVE;
        return transfer(d, 0, status_type, 0);
    default: return 0;
    }
}
static int control_complete(struct dwc3 *d, unsigned ep, uint32_t residual)
{
    switch (d->control_state) {
    case CTRL_SETUP:
        if (ep != 0 || residual) return stall(d);
        sync(d, buffer(d, 0), EP_BUFFER_BYTES, DWC3_FROM_DEVICE);
        return setup_request(d);
    case CTRL_IN_ACTIVE:
        if (ep != 1 || residual) return stall(d);
        d->control_state = CTRL_STATUS_OUT_WAIT; return 0;
    case CTRL_OUT_ACTIVE:
        if (ep != 0 || residual != 64u - d->control_length) return stall(d);
        sync(d, buffer(d, 0), EP_BUFFER_BYTES, DWC3_FROM_DEVICE);
        copy(d->line_coding, buffer(d, 0), 7);
        d->control_state = CTRL_STATUS_IN_WAIT; return 0;
    case CTRL_STATUS_IN_ACTIVE:
    case CTRL_STATUS_OUT_ACTIVE: return setup_phase(d);
    default: return stall(d);
    }
}
static int pump(struct dwc3 *d)
{
    uint32_t n, i;
    if (!d->configured) return 0;
    /* Keep one bulk-OUT transfer armed. Completed bytes are copied into the
     * finite RX ring by handle_event before this function rearms it. */
    if (!d->active[EP_OUT] && transfer(d, EP_OUT, DWC3_TRBCTL_NORMAL, EP_BUFFER_BYTES)) return -4;
    if (!d->dtr || d->active[EP_IN]) return 0;
    n = d->tx_used < EP_BUFFER_BYTES ? d->tx_used : EP_BUFFER_BYTES;
    if (!n && !d->zlp_pending) return 0;
    for (i = 0; i < n; i++) buffer(d, EP_IN)[i] = d->tx[(d->tx_tail + i) % TX_BYTES];
    if (transfer(d, EP_IN, DWC3_TRBCTL_NORMAL, n)) return -4;
    d->in_length = n;
    return 0;
}
static int device_event(struct dwc3 *d, unsigned event)
{
    unsigned i, speed;
    trace(d, DWC3_TRACE_DEVICE_EVENT, event, rd(d, DWC3_DSTS));
    if (event == DWC3_DEVT_USBRST || event == DWC3_DEVT_DISCONN) {
        d->configured = d->dtr = d->zlp_pending = 0;
        rx_reset(d);
        if (end_noncontrol(d)) return -4;
        /* EP0 SETUP/STATUS have special hardware rules; only an active DATA
         * transfer needs ENDTRANSFER before resetting the control state. */
        if (d->control_state == CTRL_IN_ACTIVE && end_transfer(d, 1)) return -4;
        if (d->control_state == CTRL_OUT_ACTIVE && end_transfer(d, 0)) return -4;
        for (i = 0; i < EP_COUNT; i++) d->active[i] = 0;
        wr(d, DWC3_DALEPENA, 3);
        wr(d, DWC3_DCFG, rd(d, DWC3_DCFG) & ~DWC3_DCFG_DEVADDR_MASK);
        d->control_state = CTRL_SETUP;
        /* The next CONNECTDONE arms a new SETUP transfer. */
        return 0;
    }
    if (event == DWC3_DEVT_CONNECTDONE) {
        speed = rd(d, DWC3_DSTS) & DWC3_DSTS_CONNECTSPD;
        if (speed != DWC3_DSTS_HIGHSPEED && speed != DWC3_DSTS_FULLSPEED1 &&
            speed != DWC3_DSTS_FULLSPEED2) return -6;
        d->full_speed = speed != DWC3_DSTS_HIGHSPEED;
        d->packet_size = d->full_speed ? 64 : 512;
        if (configure_ep(d, 0, DWC3_DEPCMD_TYPE_CONTROL, 64, 1) ||
            configure_ep(d, 1, DWC3_DEPCMD_TYPE_CONTROL, 64, 1) ||
            configure_ep(d, EP_IN, DWC3_DEPCMD_TYPE_BULK, d->packet_size, 1) ||
            configure_ep(d, EP_OUT, DWC3_DEPCMD_TYPE_BULK, d->packet_size, 1)) return -4;
        return setup_phase(d);
    }
    if (event == DWC3_DEVT_EVNTOVERFLOW || event == DWC3_DEVT_ERRTICERR) return -7;
    return 0;
}
static int handle_event(struct dwc3 *d, uint32_t event)
{
    unsigned ep, type, status;
    uint32_t residual;
    if (event & 1u) {
        if (((event >> 1) & 127u) == 0) return device_event(d, (event >> 8) & 15u);
        trace(d, DWC3_TRACE_BAD_EVENT, event, 0); return 0;
    }
    ep = (event >> 1) & 31u;
    type = (event >> 6) & 15u;
    status = (event >> 12) & 15u;
    if (ep >= EP_COUNT || ep == 2) {
        trace(d, DWC3_TRACE_BAD_EVENT, event, 1); return 0;
    }
    if (type == DWC3_DEPEVT_XFERCOMPLETE) {
        if (!d->active[ep]) return 0;
        sync(d, trb(d, ep), 64, DWC3_FROM_DEVICE);
        residual = trb(d, ep)->size & DWC3_TRB_SIZE_MASK;
        d->active[ep] = 0;
        if (status & DEPEVT_STATUS_BUSERR) return -8;
        if (ep < 2 && DWC3_TRB_SIZE_TRBSTS(trb(d, ep)->size) == DWC3_TRBSTS_SETUP_PENDING)
            return setup_phase(d);
        if (ep < 2) return control_complete(d, ep, residual);
        if (DWC3_TRB_SIZE_TRBSTS(trb(d, ep)->size) != DWC3_TRBSTS_OK) return -8;
        if (ep == EP_OUT) {
            if (residual > EP_BUFFER_BYTES) return -8;
            sync(d, buffer(d, EP_OUT), EP_BUFFER_BYTES, DWC3_FROM_DEVICE);
            rx_push(d, buffer(d, EP_OUT), EP_BUFFER_BYTES - residual);
        }
        if (ep == EP_IN) {
            if (residual > d->in_length) return -8;
            d->tx_tail = (d->tx_tail + d->in_length - residual) % TX_BYTES;
            d->tx_used -= d->in_length - residual;
            d->zlp_pending = d->in_length != 0 && residual == 0 &&
                             (d->in_length % d->packet_size) == 0;
        }
    } else if (type == DWC3_DEPEVT_XFERNOTREADY && !d->active[ep] && ep < 2) {
        return control_not_ready(d, ep, status);
    }
    return 0;
}

size_t dwc3_context_bytes(void) { return sizeof(struct dwc3); }
int dwc3_init(void *context, size_t context_bytes,
                  const struct dwc3_platform *platform,
                  const struct dwc3_identity *identity,
                  void *dma_cpu, uint64_t dma_device, size_t dma_bytes)
{
    struct dwc3 *d = context;
    unsigned ep, interrupts, total_eps, in_eps;
    uint32_t id, params;
    size_t manufacturer_chars, product_chars, serial_chars;
    if (!context || ((uintptr_t)context & 7u) || context_bytes < sizeof(*d) ||
        !platform || !platform->read32 || !platform->write32 || !platform->delay_us ||
        !platform->dma_sync || !platform->barrier || !dma_cpu ||
        ((uintptr_t)dma_cpu & (DWC3_DMA_ALIGNMENT - 1)) ||
        (dma_device & (DWC3_DMA_ALIGNMENT - 1)) ||
        dma_device > UINT64_MAX - DWC3_DMA_BYTES || dma_bytes < DWC3_DMA_BYTES)
        return -1;
    if (!identity || !identity->vid || !identity->pid ||
        !(manufacturer_chars = identity_chars(identity->manufacturer)) ||
        !(product_chars = identity_chars(identity->product)) ||
        !(serial_chars = identity_chars(identity->serial))) return -1;
    zero(d, sizeof(*d)); copy(&d->io, platform, sizeof(*platform));
    copy(d->device_descriptor, device_descriptor, sizeof(device_descriptor));
    d->device_descriptor[8] = (uint8_t)identity->vid;
    d->device_descriptor[9] = (uint8_t)(identity->vid >> 8);
    d->device_descriptor[10] = (uint8_t)identity->pid;
    d->device_descriptor[11] = (uint8_t)(identity->pid >> 8);
    string_descriptor(d->manufacturer, identity->manufacturer, manufacturer_chars);
    string_descriptor(d->product, identity->product, product_chars);
    string_descriptor(d->serial, identity->serial, serial_chars);
    d->dma_cpu = dma_cpu; d->dma_device = dma_device;
    d->packet_size = 512;
    d->line_coding[0] = 0; d->line_coding[1] = 0xc2;
    d->line_coding[2] = 1; d->line_coding[6] = 8;
    id = rd(d, DWC3_GSNPSID); trace(d, DWC3_TRACE_ID, id, 0);
    if ((id & 0xffff0000u) != 0x55330000u && (id & 0xffff0000u) != 0x33310000u)
        return -2;
    d->usb31 = (id & 0xffff0000u) == 0x33310000u;
    params = rd(d, DWC3_GHWPARAMS3);
    total_eps = (params >> 12) & 63u;
    in_eps = (params >> 18) & 31u;
    if (total_eps < EP_COUNT || in_eps < 3 || in_eps > total_eps || total_eps - in_eps < 3)
        return -2;
    interrupts = (rd(d, DWC3_GHWPARAMS1) >> 15) & 63u;
    if (!interrupts || interrupts > 32) return -2;
    wr(d, DWC3_DCTL, rd(d, DWC3_DCTL) & ~DWC3_DCTL_RUN_STOP);
    if (wait_mask(d, DWC3_DSTS, DWC3_DSTS_DEVCTRLHLT, DWC3_DSTS_DEVCTRLHLT)) return -3;
    wr(d, DWC3_DCTL, rd(d, DWC3_DCTL) | DWC3_DCTL_CSFTRST);
    if (wait_mask(d, DWC3_DCTL, DWC3_DCTL_CSFTRST, 0)) return -3;
    wr(d, DWC3_DEVTEN, 0); wr(d, DWC3_DALEPENA, 0);
    /* A previous USB owner may have left other interrupters and unacknowledged
     * event counts. Halt precedes replacing any event-DMA mapping. */
    for (ep = 0; ep < interrupts; ep++) {
        wr(d, DWC3_GEVNTSIZ(ep), 1u << 31);
        params = rd(d, DWC3_GEVNTCOUNT(ep)) & DWC3_GEVNTCOUNT_MASK;
        wr(d, DWC3_GEVNTCOUNT(ep), params);
        wr(d, DWC3_GEVNTADRLO(ep), 0); wr(d, DWC3_GEVNTADRHI(ep), 0);
    }
    zero(dma_cpu, DWC3_DMA_BYTES);
    sync(d, dma_cpu, DWC3_DMA_BYTES, DWC3_TO_DEVICE);
    wr(d, DWC3_GCTL, (rd(d, DWC3_GCTL) & ~(DWC3_GCTL_PRTCAPDIR(3) |
         DWC3_GCTL_GBLHIBERNATIONEN)) | DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_DEVICE));
    wr(d, DWC3_DCFG, (rd(d, DWC3_DCFG) & ~(DWC3_DCFG_SPEED_MASK | DWC3_DCFG_DEVADDR_MASK |
         DWC3_DCFG_LPM_CAP)) | DWC3_DCFG_HIGHSPEED);
    wr(d, DWC3_GEVNTADRLO(0), (uint32_t)dma_device);
    wr(d, DWC3_GEVNTADRHI(0), (uint32_t)(dma_device >> 32));
    /* Mask the event IRQ; status still accumulates for polling. */
    wr(d, DWC3_GEVNTSIZ(0), EVENT_BYTES | (1u << 31));
    wr(d, DWC3_GEVNTCOUNT(0), 0);
    wr(d, DWC3_DEVTEN, DWC3_DEVTEN_DISCONNEVTEN | DWC3_DEVTEN_USBRSTEN |
         DWC3_DEVTEN_CONNECTDONEEN | DWC3_DEVTEN_EVNTOVERFLOWEN | DWC3_DEVTEN_ERRTICERREN);
    if (ep_command(d, 0, DWC3_DEPCMD_DEPSTARTCFG, 0, 0, 0)) goto fail;
    for (ep = 0; ep < EP_COUNT; ep++) {
        if (ep == 2) continue;
        unsigned type = ep < 2 ? DWC3_DEPCMD_TYPE_CONTROL :
                        ep == EP_NOTIFY ? DWC3_DEPCMD_TYPE_INTR : DWC3_DEPCMD_TYPE_BULK;
        unsigned mps = ep < 2 ? 64 : ep == EP_NOTIFY ? 16 : 512;
        if (configure_ep(d, ep, type, mps, 0)) goto fail;
    }
    wr(d, DWC3_DALEPENA, 3);
    d->initialized = 1;
    return 0;
fail:
    wr(d, DWC3_DEVTEN, 0); wr(d, DWC3_DALEPENA, 0); return -4;
}
int dwc3_start(struct dwc3 *d)
{
    if (!d || !d->initialized || d->failed) return -1;
    wr(d, DWC3_DCTL, rd(d, DWC3_DCTL) | DWC3_DCTL_RUN_STOP);
    d->running = 1; return 0;
}
int dwc3_poll(struct dwc3 *d, unsigned budget)
{
    uint32_t bytes, event;
    unsigned done = 0;
    int ret = 0;
    if (!d || !d->running || d->failed) return -1;
    if (budget > 256) budget = 256;
    bytes = rd(d, DWC3_GEVNTCOUNT(0)) & DWC3_GEVNTCOUNT_MASK;
    if (bytes > EVENT_BYTES) { d->failed = -7; return -7; }
    while (bytes >= 4 && done < budget) {
        uint8_t *p = d->dma_cpu + d->event_offset;
        sync(d, p, 4, DWC3_FROM_DEVICE);
        event = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                ((uint32_t)p[3] << 24);
        d->event_offset = (d->event_offset + 4) % EVENT_BYTES;
        ret = handle_event(d, event);
        d->events++; d->last_event = event;
        wr(d, DWC3_GEVNTCOUNT(0), 4);
        done++; bytes -= 4;
        if (ret) { d->failed = ret; return ret; }
    }
    ret = pump(d);
    if (ret) { d->failed = ret; return ret; }
    return (int)done;
}
size_t dwc3_queue(struct dwc3 *d, const void *bytes, size_t count)
{
    const uint8_t *p = bytes;
    size_t n = count, i;
    if (!d || !d->initialized || !bytes) return 0;
    if (n > TX_BYTES - d->tx_used) n = TX_BYTES - d->tx_used;
    for (i = 0; i < n; i++) { d->tx[d->tx_head] = p[i]; d->tx_head = (d->tx_head + 1) % TX_BYTES; }
    d->tx_used += (uint32_t)n;
    d->dropped += count - n;
    return n;
}
size_t dwc3_read(struct dwc3 *d, void *bytes, size_t count)
{
    uint8_t *to = bytes;
    size_t n, i;
    if (!d || !bytes || !count) return 0;
    n = count < d->rx_used ? count : d->rx_used;
    for (i = 0; i < n; i++) {
        to[i] = d->rx[d->rx_tail];
        d->rx_tail = (d->rx_tail + 1u) % DWC3_RX_BYTES;
    }
    d->rx_used -= (uint32_t)n;
    return n;
}
int dwc3_ready(const struct dwc3 *d) { return d && d->running && !d->failed && d->configured && d->dtr; }
size_t dwc3_queued(const struct dwc3 *d) { return d ? d->tx_used : 0; }
uint64_t dwc3_dropped(const struct dwc3 *d) { return d ? d->dropped : 0; }
size_t dwc3_rx_pending(const struct dwc3 *d) { return d ? d->rx_used : 0; }
uint64_t dwc3_rx_overflow(const struct dwc3 *d) { return d ? d->rx_overflow : 0; }
uint32_t dwc3_rx_generation(const struct dwc3 *d) { return d ? d->rx_generation : 0; }
void dwc3_snapshot(struct dwc3 *d, struct dwc3_snapshot *s)
{
    unsigned ep;
    if (!d || !s) return;
    zero(s, sizeof(*s));
    s->events = d->events; s->commands = d->commands; s->last_event = d->last_event;
    s->event_offset = d->event_offset; s->failed = d->failed;
    s->snpsid = rd(d, DWC3_GSNPSID); s->gctl = rd(d, DWC3_GCTL);
    s->dcfg = rd(d, DWC3_DCFG); s->dctl = rd(d, DWC3_DCTL); s->dsts = rd(d, DWC3_DSTS);
    s->event_count = rd(d, DWC3_GEVNTCOUNT(0)); s->enabled_endpoints = rd(d, DWC3_DALEPENA);
    s->configured = d->configured; s->dtr = d->dtr; s->queued = d->tx_used;
    s->rx_pending = d->rx_used; s->rx_generation = d->rx_generation;
    s->rx_overflow = d->rx_overflow;
    for (ep = 0; ep < EP_COUNT; ep++) if (d->active[ep]) s->active_endpoints |= 1u << ep;
}
int dwc3_stop(struct dwc3 *d)
{
    unsigned ep;
    if (!d || !d->initialized) return -1;
    d->dtr = d->configured = 0;
    rx_reset(d);
    for (ep = 0; ep < EP_COUNT; ep++) {
        if (ep < 2 && d->control_state != CTRL_IN_ACTIVE && d->control_state != CTRL_OUT_ACTIVE)
            continue;
        if (end_transfer(d, ep)) return -4;
    }
    wr(d, DWC3_DEVTEN, 0); wr(d, DWC3_DALEPENA, 0);
    wr(d, DWC3_DCTL, rd(d, DWC3_DCTL) & ~DWC3_DCTL_RUN_STOP);
    if (wait_mask(d, DWC3_DSTS, DWC3_DSTS_DEVCTRLHLT, DWC3_DSTS_DEVCTRLHLT)) return -3;
    d->running = 0; return 0;
}
