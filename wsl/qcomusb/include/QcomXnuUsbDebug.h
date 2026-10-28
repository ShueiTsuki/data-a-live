/* SPDX-License-Identifier: MIT */
#ifndef QCOM_XNU_USB_DEBUG_H
#define QCOM_XNU_USB_DEBUG_H
#include <stddef.h>
#include <stdint.h>
#include "QcomUsbScreenshot.h"

enum qcom_usb_debug_phase {
    QCOM_USB_DEBUG_NOT_INITIALIZED,
    QCOM_USB_DEBUG_DISABLED,
    QCOM_USB_DEBUG_PREPARING,
    QCOM_USB_DEBUG_ACTIVE,
    QCOM_USB_DEBUG_STOPPED,
    QCOM_USB_DEBUG_REFUSED,
    QCOM_USB_DEBUG_IO_FAILED
};

enum qcom_usb_debug_error {
    QCOM_USB_DEBUG_OK,
    QCOM_USB_DEBUG_CONTEXT_TOO_SMALL,
    QCOM_USB_DEBUG_BAD_COUNTER,
    QCOM_USB_DEBUG_CACHE_GRANULE_UNSUPPORTED,
    QCOM_USB_DEBUG_DMA_NOT_CONTIGUOUS,
    QCOM_USB_DEBUG_MAP_FAILED,
    QCOM_USB_DEBUG_INHERITED_STATE_REFUSED,
    QCOM_USB_DEBUG_UNSTABLE_POWER_STATE,
    QCOM_USB_DEBUG_CORE_NOT_DEVICE,
    QCOM_USB_DEBUG_HALT_TIMEOUT,
    QCOM_USB_DEBUG_DMA_REFUSED,
    QCOM_USB_DEBUG_CORE_INIT_FAILED,
    QCOM_USB_DEBUG_CORE_START_FAILED,
    QCOM_USB_DEBUG_CORE_POLL_FAILED,
    QCOM_USB_DEBUG_DEADLINE_EXPIRED,
    QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED,
    QCOM_USB_DEBUG_RESTORE_FAILED,
    QCOM_USB_DEBUG_VBUS_FAILED,
    QCOM_USB_DEBUG_DMA_WIDTH_UNSUPPORTED,
    QCOM_USB_DEBUG_TABLES_NOT_CONTIGUOUS,
    QCOM_USB_DEBUG_DOMAIN_STATE_INVALID,
    QCOM_USB_DEBUG_ACQUIRE_REFUSED,
    QCOM_USB_DEBUG_PHY_LEASE_FAILED,
    QCOM_USB_DEBUG_PREPARE_FAILED
};

/* Opt-in preparing-panic delay is 100 + 2*checkpoint seconds. */
enum qcom_usb_debug_diag_checkpoint {
    QCOM_USB_DEBUG_DIAG_NONE,
    QCOM_USB_DEBUG_DIAG_BEFORE_ACQUIRE,
    QCOM_USB_DEBUG_DIAG_AFTER_ACQUIRE,
    QCOM_USB_DEBUG_DIAG_COUNTER_CACHE,
    QCOM_USB_DEBUG_DIAG_DATA_VTOP,
    QCOM_USB_DEBUG_DIAG_TABLE_VTOP,
    QCOM_USB_DEBUG_DIAG_MAP_GCC,
    QCOM_USB_DEBUG_DIAG_MAP_USB,
    QCOM_USB_DEBUG_DIAG_MAP_SMMU,
    QCOM_USB_DEBUG_DIAG_INSPECT,
    QCOM_USB_DEBUG_DIAG_GATES_WIDTH,
    QCOM_USB_DEBUG_DIAG_HALT,
    QCOM_USB_DEBUG_DIAG_DOMAIN,
    QCOM_USB_DEBUG_DIAG_VERIFY_DOMAIN,
    QCOM_USB_DEBUG_DIAG_VBUS,
    QCOM_USB_DEBUG_DIAG_CORE_INIT,
    QCOM_USB_DEBUG_DIAG_CORE_START,
    QCOM_USB_DEBUG_DIAG_CORE_STARTED,
    QCOM_USB_DEBUG_DIAG_FIRST_POLL,
    QCOM_USB_DEBUG_DIAG_ENUM_DONE
};

enum qcom_usb_debug_dma_mode {
    QCOM_USB_DEBUG_DMA_NONE = 0,
    QCOM_USB_DEBUG_DMA_PRIVATE_TRANSLATED = 16,
    QCOM_USB_DEBUG_DMA_INHERITED_IDENTITY = 17,
    QCOM_USB_DEBUG_DMA_BYPASS_BANK = 18
};

typedef struct qcom_usb_debug_status {
    uint32_t phase, error;
    int32_t core_result;
    uint32_t qcom_result, restore_result, dma_mode, dma_lease_active;
    uint32_t vbus_owned, vbus_saved;
    uint32_t phy_owned, phy_saved, phy_applied, phy_restoring, stop_failed;
    uint32_t dma_address_width, hwparams0;
    uint32_t last_trace, trace_a, trace_b, traces;
    uint32_t poll_calls, last_poll_events, io_failed;
    uint64_t dma_pa, counter_frequency, cache_line, dropped;
    uint32_t domain_result, domain_stage, domain_bank, domain_slot, domain_asid;
    uint64_t tables_pa, dma_iova, domain_last_address, domain_last_expected, domain_last_readback;
    uint32_t diag_enabled, diag_checkpoint;
    uint64_t buffer_capacity, buffer_pending, buffer_high_water, buffer_accepted;
    uint64_t buffer_overflow, buffer_contended, buffer_replayed, transport_pending;
    uint64_t rx_overflow, control_replies;
    uint32_t rx_pending, rx_generation;
    uint32_t control_enabled, control_hook_owned, control_reboot_state, control_rejected;
    /* qcom-usb-prepare=1 only: how far the device-mode bring-up got. */
    uint32_t prepare_result, prepare_stage, prepare_readback;
} qcom_usb_debug_status;

#ifdef __cplusplus
extern "C" {
#endif

/* Call once on the boot CPU, after arm_vm_init and ExitBootServices, at or
 * before PE_init_kprintf. Requires exclusive USB ownership and no competing
 * firmware callbacks. Returns 1 only when the fresh native core is running.
 * Explicit qcom-usb-debug=1 is mandatory. No hook is installed here. */
/* Data A Live: map the register windows first, interrupts enabled (a running kernel). */
int qcom_usb_debug_map(void);
int qcom_usb_debug_init(void);
/* Candidate PE_kputc sink. All CPUs can enqueue before/during initialization.
 * IRQ-safe nonspinning CPU-only backlog preserves the oldest bytes; overflow
 * and producer contention are counted. Hardware service stays on the boot CPU.
 * A write accepts at most 16KiB; excess is counted and the prefix is returned.
 * No console, printf, allocation, scheduler or EFI functions are called. */
void qcom_usb_debug_putc(char c);
size_t qcom_usb_debug_write(const void *bytes, size_t count);
/* At most one USB event and a bounded pump; schedule externally if needed. */
void qcom_usb_debug_poll(void);
/* Worker-only opt-in. A qualified SoC profile may install the final PSCI
 * restart callback once, only when PE_halt_restart is NULL. No command runs. */
int qcom_usb_debug_control_start(void);
/* One-shot handoff from the USB worker to a dedicated normal reboot thread. */
int qcom_usb_debug_control_take_reboot(void);
/* Consume the final restart request only after the ACK and TX backlog drained. */
int qcom_usb_debug_control_arm_reboot(void);
void qcom_usb_debug_control_reboot_failed(int result);
/* Bounded normal-thread drain wait used before reboot_kernel(0, NULL). */
int qcom_usb_debug_wait_drained(unsigned int timeout_ms);
/* Data A Live: the same from any CPU, right before the reset, once the serial thread has stopped. */
int qcom_usb_debug_final_drain(unsigned int timeout_ms);
/* Early PREPARING diagnosis only; inactive/ACTIVE calls return promptly. */
void qcom_usb_debug_panic_prepare(void);
/* Tries to drain messages and an end marker for at most two seconds using
 * CNTVCT. Explicit reset diagnostics can reboot afterwards; normal mode
 * neither reboots nor stops XNU. Recursive calls return immediately. */
void qcom_usb_debug_panic_drain(void);
/* On success the core is halted and any exact-stream lease restored. Static
 * DMA arena remains resident even after failure; no allocation is freed. */
int qcom_usb_debug_stop(void);
void qcom_usb_debug_get_status(struct qcom_usb_debug_status *status);
#ifdef __cplusplus
}
#endif
#endif
