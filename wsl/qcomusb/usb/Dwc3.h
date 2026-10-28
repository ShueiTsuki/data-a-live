/* SPDX-License-Identifier: MIT */
#ifndef DWC3_H
#define DWC3_H
#include <stddef.h>
#include <stdint.h>

/* One caller-owned, contiguous device-DMA mapping. Never assume DMA == PA. */
#define DWC3_DMA_BYTES 32768u
#define DWC3_DMA_ALIGNMENT 4096u
#define DWC3_CONTEXT_ALIGNMENT 8u
#define DWC3_CONTEXT_BYTES 20480u
#define DWC3_TX_BYTES 16384u
#define DWC3_RX_BYTES 512u

/* Copied at init; no SoC configuration, OS headers or platform IDs here.
 * Each string must contain 1..31 printable ASCII characters. */
struct dwc3_identity {
    uint16_t vid, pid;
    const char *manufacturer, *product, *serial;
};

enum dwc3_sync_direction {
    DWC3_TO_DEVICE,
    DWC3_FROM_DEVICE
};

enum dwc3_trace {
    DWC3_TRACE_ID = 1,
    DWC3_TRACE_TIMEOUT,
    DWC3_TRACE_COMMAND,
    DWC3_TRACE_DEVICE_EVENT,
    DWC3_TRACE_SETUP,
    DWC3_TRACE_UNSUPPORTED,
    DWC3_TRACE_BAD_EVENT,
    DWC3_TRACE_READY
};

struct dwc3_platform {
    void *cookie;
    uint32_t (*read32)(void *cookie, uint32_t offset);
    void (*write32)(void *cookie, uint32_t offset, uint32_t value);
    void (*delay_us)(void *cookie, uint32_t usec);
    /* Must also handle noncoherent DMA. Event/TRB cache lines are separate. */
    void (*dma_sync)(void *cookie, void *cpu, size_t bytes,
                     enum dwc3_sync_direction direction);
    void (*barrier)(void *cookie);
    void (*trace)(void *cookie, enum dwc3_trace event, uint32_t a, uint32_t b);
};

struct dwc3;
size_t dwc3_context_bytes(void);
/* Platform must have already established device role, PHY/clocks, power and
 * SMMU mapping. This resets only the DWC device side; it never initializes PHY.
 * Buffers must remain mapped at the same CPU/DMA addresses for this instance.
 * Returns 0 or a negative code. On failure controller is left stopped. */
int dwc3_init(void *context, size_t context_bytes,
                  const struct dwc3_platform *platform,
                  const struct dwc3_identity *identity,
                  void *dma_cpu, uint64_t dma_device, size_t dma_bytes);
int dwc3_start(struct dwc3 *dev);
/* Each poll is bounded to min(budget, 256) events. Call from one CPU only;
 * no interrupts, allocation, scheduler, EFI or platform-library calls occur. */
int dwc3_poll(struct dwc3 *dev, unsigned int budget);
/* Nonblocking ring; accepts early messages before the host opens the port. */
size_t dwc3_queue(struct dwc3 *dev, const void *bytes, size_t count);
/* Nonblocking bulk-OUT receive ring. The caller must serialize this with poll
 * on the same CPU. A device reset/disconnect clears pending RX bytes and
 * advances rx_generation so a protocol parser can invalidate its state. */
size_t dwc3_read(struct dwc3 *dev, void *bytes, size_t count);
int dwc3_ready(const struct dwc3 *dev);
size_t dwc3_queued(const struct dwc3 *dev);
uint64_t dwc3_dropped(const struct dwc3 *dev);
size_t dwc3_rx_pending(const struct dwc3 *dev);
uint64_t dwc3_rx_overflow(const struct dwc3 *dev);
uint32_t dwc3_rx_generation(const struct dwc3 *dev);
struct dwc3_snapshot {
    uint32_t events, commands, last_event, event_offset;
    uint32_t snpsid, gctl, dcfg, dctl, dsts, event_count, enabled_endpoints;
    uint32_t configured, dtr, queued, active_endpoints;
    uint32_t rx_pending, rx_generation;
    uint64_t rx_overflow;
    int32_t failed;
};
/* Read-only diagnostics, for callers that already own powered MMIO access. */
void dwc3_snapshot(struct dwc3 *dev, struct dwc3_snapshot *snapshot);
/* Stops DMA. Do not release mapping until this succeeds. */
int dwc3_stop(struct dwc3 *dev);

/* Descriptor view for host conformance tests and the firmware USB backend.
 * type uses USB descriptor IDs; full_speed selects bulk wMaxPacketSize=64. */
const void *dwc3_descriptor(const struct dwc3 *dev, unsigned int type, unsigned int index,
                               int full_speed, size_t *length);
#endif
