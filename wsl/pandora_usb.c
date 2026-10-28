/*
 * PANDORA USB. pandora_usb=2 (with qcom-usb-debug=1): the console log as a USB serial port (CDC
 * ACM; Windows binds usbser, a COM port) through QcomXnuUsbDebug (bsd/dev/qcomusb, from
 * nabu-usb-cdc's module, profile soc/Sm8850.h). The bootloader hands the controller over clocked,
 * in device role and halted (usb1), which is the state the module takes over without bringing
 * anything up. A thread bound to the boot CPU (the module serves the hardware from there only)
 * starts the module, then every PU_PERIOD_MS hands it the console ring's new bytes
 * (sweet_log_copy, from the start of the ring, so the boot is included) and polls the
 * controller, which completes enumeration and moves data. Bytes the module cannot take yet are
 * offered again; a reader that falls a whole ring behind skips ahead (sweet_log_copy).
 * pandora_usb=1, read only: the state the bootloader leaves the primary USB controller in when
 * it hands over, which decides what the serial console has to bring up itself.
 * - GCC (gcc-kaanapali.c): the five clocks of the live DT's ssusb@a600000 (core = usb30_prim_master
 *   0x39018, iface = cfg_noc_usb3_prim_axi 0x39090, bus_aggr = aggre_usb3_prim_axi 0x39094, utmi =
 *   usb30_prim_mock_utmi 0x3902c, sleep = usb30_prim_sleep 0x39028; CLK_OFF bit 31, enable bit 0),
 *   the controller's reset (BCR 0x39000) and power domain (GDSCR 0x39004, PWR_ON bit 31;
 *   CFG_GDSCR 0x39008).
 * - Only with the core and iface clocks running and the power domain on: the DWC3 (0xa600000)
 *   identity, global, device and event-buffer registers, twice 100 ms apart (an event count or
 *   state that moves means someone still runs it), and the Qualcomm QSCRATCH registers.
 * The PHY (eUSB2, 0x88e3000) is not read: its clocks are not ours to check here. The apps SMMU's
 * entry for the USB stream (0x40) is already known from the audio survey: group 3 -> bank 3,
 * translation off (spk6), so the controller's DMA sees physical addresses.
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <kern/cpu_number.h>
#include <kern/processor.h>
#include "qcomusb/include/QcomXnuUsbDebug.h"

extern kern_return_t thread_terminate(thread_t);
extern uint32_t sweet_log_copy(uint64_t *pos, char *out, uint32_t max);
/* osfmk's, not in the headers BSD code sees (kern/processor.h, kern/sched_prim.h); master_processor
 * itself is a per-CPU macro there */
extern processor_t cpu_to_processor(int cpu);
extern processor_t thread_bind(processor_t processor);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
void pandora_usb_init(void);

#define PU_GCC_PA     0x00139000u      /* GCC 0x100000 + the USB30_PRIM block */
#define PU_DWC3_PA    0x0a60c000u      /* DWC3 global and device registers (0xc000-0xcfff) */
#define PU_QSCR_PA    0x0a6f8000u      /* QSCRATCH at 0xf8800 */
#define PU_CLK_OFF    (1u << 31)
#define PU_PWR_ON     (1u << 31)

static const struct {
	uint32_t off;
	const char *name;
} pu_clk[5] = {{0x018, "core"}, {0x090, "iface"}, {0x094, "aggr"}, {0x02c, "utmi"}, {0x028, "sleep"}};

static uint32_t
pu_rd(vm_offset_t base, uint32_t off)
{
	const uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
pu_dwc3(vm_offset_t d, vm_offset_t q, const char *when)
{
	/* offsets from 0xa600000, minus the 0xc000 the mapping starts at */
	printf("PANDORA USB: DWC3 %s: GSNPSID %08x GCTL %08x GSTS %08x GUSB2PHYCFG %08x GUSB3PIPECTL %08x DCFG %08x "
	    "DCTL %08x DEVTEN %08x DSTS %08x DALEPENA %08x GEVNT %08x:%08x size %08x count %08x\n", when,
	    pu_rd(d, 0x120), pu_rd(d, 0x110), pu_rd(d, 0x118), pu_rd(d, 0x200), pu_rd(d, 0x2c0), pu_rd(d, 0x700),
	    pu_rd(d, 0x704), pu_rd(d, 0x708), pu_rd(d, 0x70c), pu_rd(d, 0x720), pu_rd(d, 0x404), pu_rd(d, 0x400),
	    pu_rd(d, 0x408), pu_rd(d, 0x40c));
	printf("PANDORA USB: QSCRATCH %s: GENERAL_CFG %08x HS_PHY_CTRL %08x SS_PHY_CTRL %08x PWR_EVNT_IRQ_STAT %08x\n", when,
	    pu_rd(q, 0x808), pu_rd(q, 0x810), pu_rd(q, 0x830), pu_rd(q, 0x858));
}

static void
pu_probe(void)
{
	vm_offset_t g = ml_io_map_unmappable(PU_GCC_PA, 0x1000, VM_WIMG_IO);
	if (!g) {
		printf("PANDORA USB: GCC not mapped\n");
		return;
	}
	uint32_t cbcr[5];
	for (unsigned i = 0; i < 5; i++) {
		cbcr[i] = pu_rd(g, pu_clk[i].off);
	}
	const uint32_t bcr = pu_rd(g, 0x000), gdscr = pu_rd(g, 0x004), cfg = pu_rd(g, 0x008);
	ml_io_unmap(g, 0x1000);
	printf("PANDORA USB: clocks core %08x iface %08x aggr %08x utmi %08x sleep %08x; BCR %08x GDSCR %08x CFG_GDSCR %08x\n",
	    cbcr[0], cbcr[1], cbcr[2], cbcr[3], cbcr[4], bcr, gdscr, cfg);
	const bool on = !(cbcr[0] & PU_CLK_OFF) && !(cbcr[1] & PU_CLK_OFF) && (gdscr & PU_PWR_ON) && !(bcr & 1u);
	if (!on) {
		printf("PANDORA USB: core or iface clock off, power domain off or reset held: the controller is not read\n");
		return;
	}
	vm_offset_t d = ml_io_map_unmappable(PU_DWC3_PA, 0x1000, VM_WIMG_IO);
	vm_offset_t q = ml_io_map_unmappable(PU_QSCR_PA, 0x1000, VM_WIMG_IO);
	if (!d || !q) {
		printf("PANDORA USB: DWC3 not mapped\n");
		return;
	}
	pu_dwc3(d, q, "at start");
	delay(100000);
	pu_dwc3(d, q, "100 ms later");
	ml_io_unmap(d, 0x1000);
	ml_io_unmap(q, 0x1000);
}

#define PU_PERIOD_MS 5u
#define PU_CHUNK     4096u

static int pu_level;
static int pu_wait;

static void
pu_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&pu_wait, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
pu_status(const char *when)
{
	struct qcom_usb_debug_status s;
	qcom_usb_debug_get_status(&s);
	printf("PANDORA USB: serial %s: phase %u error %u core %d qcom %u restore %u dma mode %u lease %u at %#llx "
	    "width %u hwparams0 %#x; domain result %u stage %u bank %u slot %u; vbus %u/%#x phy %u/%u; polls %u, "
	    "dropped %llu, io failed %u\n", when, s.phase, s.error, s.core_result, s.qcom_result, s.restore_result,
	    s.dma_mode, s.dma_lease_active, s.dma_pa, s.dma_address_width, s.hwparams0, s.domain_result,
	    s.domain_stage, s.domain_bank, s.domain_slot, s.vbus_owned, s.vbus_saved, s.phy_owned, s.phy_applied,
	    s.poll_calls, s.dropped, s.io_failed);
}

/* The serial console: started and served from the boot CPU. */
static void
pu_serial(void)
{
	(void)thread_bind(cpu_to_processor(master_cpu));
	thread_block(THREAD_CONTINUE_NULL);
	if (cpu_number() != master_cpu) {
		printf("PANDORA USB: serial not started: not on the boot CPU\n");
		return;
	}
	/* the module masks interrupts while it works; its windows are mapped first, here */
	if (!qcom_usb_debug_map()) {
		printf("PANDORA USB: serial not started: register windows not mapped\n");
		return;
	}
	const int up = qcom_usb_debug_init();
	pu_status(up ? "started" : "refused");
	if (!up) {
		return;
	}
	static char chunk[PU_CHUNK];
	uint64_t pos = 0;
	uint32_t held = 0, sent = 0;    /* chunk bytes not yet taken; of them already taken */
	for (unsigned n = 1;; n++) {
		if (held == sent) {
			held = sweet_log_copy(&pos, chunk, PU_CHUNK);
			sent = 0;
		}
		if (held > sent) {
			sent += (uint32_t)qcom_usb_debug_write(chunk + sent, held - sent);
		}
		qcom_usb_debug_poll();
		if (n == 2000 || n % 60000 == 0) {    /* 10 s after the start, then every 5 min */
			pu_status("running");
		}
		pu_sleep(PU_PERIOD_MS);
	}
}

static void
pu_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	if (pu_level >= 2) {
		pu_serial();
	} else {
		pu_probe();
	}
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
pandora_usb_init(void)
{
	(void)PE_parse_boot_argn("pandora_usb", &pu_level, sizeof(pu_level));
	if (!pu_level) {
		return;
	}
	thread_t thread;
	if (kernel_thread_start(pu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
