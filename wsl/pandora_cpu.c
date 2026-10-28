/*
 * PANDORA CPU performance: what the cores run at and what the firmware offers (patch 0200), boot-arg
 * pandora_cpu (default 1; 0 = off). Read only: nothing is changed.
 *
 * On this SoC the CPU frequencies are set by the CPUCP firmware. Android asks it through SCMI (Arm's System
 * Control and Management Interface, performance protocol 0x13) over the "pdp" channel (scmi-cpufreq, governor
 * walt): device tree qcom,pdp-scmi, mailbox qcom,pdp0 (qcom,cpucp-v2) with its doorbell at 0x19980000 + 0x100 +
 * channel * 8 + 4 (Linux drivers/mailbox/qcom-cpucp-mbox.c), the request area at 0x81f08100 and the
 * notification area at 0x81f08000 (pdp-sram in the reserved pdp_ns_shared_region, 0x80 bytes each, the
 * arm,scmi-shmem layout of drivers/firmware/arm_scmi/shmem.c). Cores 0-5 are performance domain 0
 * (384 MHz - 3.63 GHz, 28 levels on Android), cores 6-7 domain 1 (768 MHz - 4.61 GHz, 27 levels).
 *
 * This probe logs, a few seconds after start:
 *   - the scheduler's view: each CPU's cluster and cluster type (ml_get_topology_info);
 *   - each cluster's clock as it runs now, from a thread bound to its first CPU timing a chain of dependent
 *     adds (one cycle each), not the PMU, which the hypervisor may trap;
 *   - SCMI performance protocol: version, domains, each domain's name, levels and limits, the current level.
 * The clock measure is repeated at 20 s and 60 s.
 *
 * Patch 0201: the governor, boot-arg pandora_cpu=2. After checking that a level set through SCMI moves the clock
 * (each domain at its top and bottom level, measured on its first CPU), every 16 ms window it takes each domain's
 * busiest CPU (idle time from recount, osfmk/arm/pandora_cpu_idle.c) and asks for the lowest level that gives
 * 1.25 x what that CPU used (Linux schedutil: next = 1.25 * busy * current), at least twice the current clock
 * when the CPU was nearly always busy (walt's hispeed jump). Up at once; down when four windows in a row want
 * less. Capped at the firmware's sustained level (the boost levels above it are left alone) and at its limits,
 * which it lowers when hot. Boot-args pandora_cpu_fix0 / pandora_cpu_fix1 = a fixed level for domain 0 / 1.
 *
 * Patch 0202 (freq2 on the phone: domain 0 follows, domain 1's level stays 8 whatever is set; the governor went
 * down to 384-900 MHz and animations felt slower than at the bootloader's 1.67 GHz; cores 6-7 showed 0-1 % busy):
 *   - domain 1 is tried four ways, each towards its own level so the clock tells which worked: a plain level set,
 *     a level set plus pdp1's doorbell (qcom,pdp1 at 0x1a980000), the SCMI fast channel the firmware describes
 *     (message 0xb), and limits set to one level (message 5). The governor then drives it the way that worked,
 *     or leaves it alone;
 *   - a floor: never below level 11 (1.67 GHz, as the bootloader left it) unless the domain has been nearly idle
 *     (under 10 % busy) for 12 windows (about 0.2 s);
 *   - every CPU's busyness in the 2 s lines.
 *
 * Patch 0203 (freq3: domain 1 follows only its fast channel, 0x1a980118 with the doorbell at 0x1a98011c; domain 0
 * has one too): domain 0 also moves to its fast channel when the firmware follows it there; domain 1's floor is
 * level 8 (1.75 GHz, as the bootloader left it). The scheduler side is osfmk/kern/sched_prim.c (pandora_bigfirst).
 *
 * Patch 0204 (freq3, freq4: since the fast channels came in, the limits read back 255 for domain 0 and 0 for
 * domain 1, and the cap at the limit only moved the governor's own record of domain 1's level to 0 every 2 s,
 * while the firmware stayed at 8-26; the next window then asked for the floor, a drop to 1.75 GHz): no cap at the
 * limits; every 2 s the firmware's current level is taken when it differs from the record.
 *
 * Patch 0205 (adds logging only): the SoC temperatures every 2 s with the governor lines; every 10 s what the
 * performance-controller callouts saw (osfmk/arm/pandora_perfctl.c: time per class on each CPU, the busiest threads
 * and processes, the frames and how many were late); every level of each domain in the probe.
 *
 * Patch 0213 (freq5: 104.8 C on a little core, 103.1 C on a big one, nothing of ours limiting): thermal control
 * from the phone's DT thermal zones (cpu_thermal below): level caps per domain and for the GPU, and hot cores
 * taken from the scheduler.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/processor.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>
#include <kern/cpu_number.h>
#include <string.h>

#if defined(PANDORA)

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void IOSleep(unsigned int milliseconds);
extern processor_t thread_bind(processor_t processor);   /* kern/sched_prim.h, not exported to BSD */
extern uint64_t pandora_cpu_idle_mach(int cpu);            /* osfmk/arm/pandora_cpu_idle.c */
extern void pandora_perfctl_start(void);                    /* osfmk/arm/pandora_perfctl.c */
extern void pandora_perfctl_log(void);
extern void pandora_idle_start(void);                       /* osfmk/arm/pandora_cpu_idle.c (0215) */
extern void pandora_prof_start(void);                       /* osfmk/arm/pandora_prof.c (0222) */
extern void pandora_autoswipe_start(void);                  /* bsd/dev/pandora_autoswipe.c (0224) */

#define PDP_MBOX_TX_PA          0x19980000ull   /* qcom,pdp0 "tx" registers */
#define PDP_MBOX_TX_CMD(ch)     (0x100u + (ch) * 8u + 4u)
#define PDP_SHMEM_PA            0x81f08000ull   /* pdp-sram: rx area at +0x000, tx area at +0x100 */
#define PDP_SHMEM_TX            0x100u
#define PDP_SHMEM_SIZE          0x80u
#define SCMI_PROTO_PERF         0x13u
#define SCMI_FREE               0x1u
#define SCMI_ERROR              0x2u

static vm_offset_t cpu_mbox, cpu_shmem, cpu_mbox1;
static uint32_t cpu_token;
#define PDP1_MBOX_TX_PA         0x1a980000ull   /* qcom,pdp1 "tx" registers */
#define CPU_IOS_RAM_PA          0x8c0000000ull  /* iOS's own memory starts here: never map a firmware address above */

static uint32_t
rd32(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static void
wr32(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

/* One SCMI command on the pdp channel, by polling the request area (channel_status at +4, flags +0x10,
 * length +0x14, header +0x18, payload +0x1c). Returns the response's 32-bit words (status first) in OUT. */
static int
scmi_call(uint32_t proto, uint32_t msg, const uint32_t *in, uint32_t nin, uint32_t *out, uint32_t maxout,
    uint32_t *nout)
{
	const vm_offset_t sh = cpu_shmem + PDP_SHMEM_TX;
	uint64_t deadline;
	*nout = 0;
	clock_interval_to_deadline(30, NSEC_PER_MSEC, &deadline);
	while (!(rd32(sh, 0x4) & SCMI_FREE)) {
		if (mach_absolute_time() > deadline) {
			return -1;   /* channel never free */
		}
	}
	const uint32_t token = (++cpu_token) & 0x3ffu;
	wr32(sh, 0x4, 0);                                   /* busy, error cleared */
	wr32(sh, 0x10, 0);                                  /* completion by polling */
	wr32(sh, 0x14, 4 + nin * 4);
	wr32(sh, 0x18, (msg & 0xff) | (proto & 0xff) << 10 | token << 18);
	for (uint32_t i = 0; i < nin; i++) {
		wr32(sh, 0x1c + i * 4, in[i]);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	wr32(cpu_mbox, PDP_MBOX_TX_CMD(0), token | 1u);     /* the doorbell; its value is not read */
	clock_interval_to_deadline(50, NSEC_PER_MSEC, &deadline);
	for (;;) {
		const uint32_t st = rd32(sh, 0x4);
		if ((st & (SCMI_FREE | SCMI_ERROR)) && ((rd32(sh, 0x18) >> 18) & 0x3ffu) == token) {
			if (st & SCMI_ERROR) {
				return -3;
			}
			break;
		}
		if (mach_absolute_time() > deadline) {
			return -2;   /* no answer */
		}
	}
	uint32_t len = rd32(sh, 0x14);
	uint32_t words = len >= 4 ? (len - 4) / 4 : 0;
	if (words > (PDP_SHMEM_SIZE - 0x1c) / 4) {
		words = (PDP_SHMEM_SIZE - 0x1c) / 4;
	}
	for (uint32_t i = 0; i < words && i < maxout; i++) {
		out[i] = rd32(sh, 0x1c + i * 4);
	}
	*nout = words < maxout ? words : maxout;
	return 0;
}

static void
scmi_name(const uint32_t *w, char name[17])
{
	memcpy(name, w, 16);
	name[16] = 0;
	for (int i = 0; i < 16 && name[i]; i++) {
		if (name[i] < 0x20 || name[i] > 0x7e) {
			name[i] = '?';
		}
	}
}

static void
cpu_scmi_probe(void)
{
	uint32_t out[32], n = 0, in[2];
	cpu_mbox = ml_io_map(PDP_MBOX_TX_PA, 0x1000);
	cpu_shmem = ml_io_map(PDP_SHMEM_PA, 0x1000);
	if (!cpu_mbox || !cpu_shmem) {
		printf("PANDORA CPU: SCMI: no mapping\n");
		return;
	}
	printf("PANDORA CPU: SCMI request area: status 0x%x flags 0x%x length %u header 0x%x\n",
	    rd32(cpu_shmem + PDP_SHMEM_TX, 0x4), rd32(cpu_shmem + PDP_SHMEM_TX, 0x10),
	    rd32(cpu_shmem + PDP_SHMEM_TX, 0x14), rd32(cpu_shmem + PDP_SHMEM_TX, 0x18));
	int r = scmi_call(SCMI_PROTO_PERF, 0x0, NULL, 0, out, 32, &n);
	if (r || n < 2) {
		printf("PANDORA CPU: SCMI perf version: error %d (%u words, status %d)\n", r, n, n ? (int)out[0] : 0);
		return;
	}
	const uint32_t version = out[1];
	printf("PANDORA CPU: SCMI perf version 0x%x (status %d)\n", version, (int)out[0]);
	r = scmi_call(SCMI_PROTO_PERF, 0x1, NULL, 0, out, 32, &n);
	const uint32_t domains = (!r && n >= 2) ? (out[1] & 0xffff) : 0;
	printf("PANDORA CPU: SCMI perf attributes: error %d status %d attributes 0x%x -> %u domains\n", r,
	    n ? (int)out[0] : 0, n >= 2 ? out[1] : 0, domains);
	for (uint32_t d = 0; d < domains && d < 4; d++) {
		char name[17] = "";
		in[0] = d;
		r = scmi_call(SCMI_PROTO_PERF, 0x3, in, 1, out, 32, &n);
		if (!r && n >= 9) {
			scmi_name(&out[5], name);
		}
		printf("PANDORA CPU: domain %u '%s': error %d status %d flags 0x%x rate limit %u us sustained %u kHz level %u\n",
		    d, name, r, n ? (int)out[0] : 0, n > 1 ? out[1] : 0, n > 2 ? out[2] : 0, n > 3 ? out[3] : 0, n > 4 ? out[4] : 0);
		/* levels: (perf value, power, latency) and from version 4 also (indicative kHz, level index) */
		const uint32_t entry = (version >> 16) >= 4 ? 5 : 3;
		uint32_t index = 0;
		for (int guard = 0; guard < 64; guard++) {   /* 0205: 16 stopped at level 15 (one level per answer) */
			in[0] = d;
			in[1] = index;
			r = scmi_call(SCMI_PROTO_PERF, 0x4, in, 2, out, 32, &n);
			if (r || n < 2 || (int)out[0] != 0) {
				printf("PANDORA CPU: domain %u levels from %u: error %d status %d\n", d, index, r, n ? (int)out[0] : 0);
				break;
			}
			const uint32_t got = out[1] & 0xfff, left = out[1] >> 16;
			for (uint32_t i = 0; i < got && 2 + (i + 1) * entry <= n; i++) {
				const uint32_t *e = &out[2 + i * entry];
				if (entry == 5) {
					printf("PANDORA CPU: domain %u level %u: perf %u power %u latency %u us freq %u kHz index %u\n", d,
					    index + i, e[0], e[1], e[2] & 0xffff, e[3], e[4]);
				} else {
					printf("PANDORA CPU: domain %u level %u: perf %u power %u latency %u us\n", d, index + i, e[0],
					    e[1], e[2] & 0xffff);
				}
			}
			index += got;
			if (!left || !got) {
				break;
			}
		}
		in[0] = d;
		r = scmi_call(SCMI_PROTO_PERF, 0x6, in, 1, out, 32, &n);
		printf("PANDORA CPU: domain %u limits: error %d status %d max %u min %u\n", d, r, n ? (int)out[0] : 0,
		    n > 1 ? out[1] : 0, n > 2 ? out[2] : 0);
		r = scmi_call(SCMI_PROTO_PERF, 0x8, in, 1, out, 32, &n);
		printf("PANDORA CPU: domain %u level now: error %d status %d level %u\n", d, r, n ? (int)out[0] : 0,
		    n > 1 ? out[1] : 0);
	}
}

/* The clock of the CPU this runs on, in MHz: dependent adds (one cycle each) over 10 ms. */
static uint32_t
cpu_clock_mhz(void)
{
	uint64_t span, start, now, x = 1, rounds = 0;
	nanoseconds_to_absolutetime(10 * NSEC_PER_MSEC, &span);
	start = mach_absolute_time();
	do {
		for (int i = 0; i < 1000; i++) {
			__asm__ volatile (
                                "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                                "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                                "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                                "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                                : "+r"(x));
		}
		rounds += 1000;
		now = mach_absolute_time();
	} while (now - start < span);
	uint64_t ns;
	absolutetime_to_nanoseconds(now - start, &ns);
	return ns ? (uint32_t)(rounds * 16 * 1000 / ns) : 0;
}

/* The clock of CPU, measured from a thread bound to it. */
static uint32_t
cpu_clock_on(int cpu)
{
	processor_t p = cpu_to_processor(cpu);
	if (p == PROCESSOR_NULL) {
		return 0;
	}
	thread_bind(p);
	thread_block(THREAD_CONTINUE_NULL);
	const uint32_t mhz = cpu_clock_mhz();
	thread_bind(PROCESSOR_NULL);
	thread_block(THREAD_CONTINUE_NULL);
	return mhz;
}

/* Each SCMI domain's first CPU (0 and 6). */
static void
cpu_clocks(const char *when)
{
	printf("PANDORA CPU: %s: cpu 0 (domain 0) about %u MHz, cpu 6 (domain 1) about %u MHz\n", when, cpu_clock_on(0),
	    cpu_clock_on(6));
}

/* ---- governor (patch 0201) ---- */
static const uint32_t cpu_khz0[] = { 384000, 537600, 691200, 787200, 883200, 998400, 1113600, 1228800, 1324800, 1440000,
	1555200, 1670400, 1785600, 1900800, 1996800, 2112000, 2227200, 2361600, 2496000, 2611200, 2745600, 2899200, 3033600,
	3187200, 3302400, 3398400, 3513600, 3628800 };
static const uint32_t cpu_khz1[] = { 768000, 883200, 1017600, 1132800, 1267200, 1382400, 1497600, 1632000, 1747200, 1862400,
	1977600, 2112000, 2227200, 2438400, 2668800, 2880000, 3129600, 3398400, 3648000, 3763200, 3859200, 3974400, 4070400,
	4185600, 4262400, 4339200, 4396800 };
struct cpu_domain {
	const uint32_t *khz;
	uint32_t levels, first, count;   /* table, CPUs first .. first + count - 1 */
	uint32_t level, top, fixed;      /* current, cap, fixed level or ~0 */
	uint32_t down_windows, want_max_down, changes, set_errors;
	uint64_t busy_sum, khz_sum, windows;
	int method;                      /* how a level is set: 0 SCMI, 1 SCMI + pdp1 doorbell, 2 fast channel, 3 limits; -1 none */
	uint32_t floor, quiet;           /* the active floor; windows in a row under 10 % busy */
	vm_offset_t fast, fast_db;       /* fast channel (method 2) and its doorbell */
	uint64_t fast_db_set, fast_db_keep;
};
static struct cpu_domain cpu_dom[2] = {
	{ cpu_khz0, sizeof(cpu_khz0) / 4, 0, 6, 0, 27, ~0u, 0, 0, 0, 0, 0, 0, 0, 0, 11, 0, 0, 0, 0, 0 },
	{ cpu_khz1, sizeof(cpu_khz1) / 4, 6, 2, 0, 26, ~0u, 0, 0, 0, 0, 0, 0, 0, -1, 8, 0, 0, 0, 0, 0 },
};
static uint64_t cpu_busy_sum[8];

static int
cpu_set_level_via(uint32_t d, uint32_t level, int method)
{
	struct cpu_domain *dm = &cpu_dom[d];
	uint32_t in[3] = { d, level, level }, out[4], n = 0;
	int r;
	switch (method) {
	case 2:
		if (!dm->fast) {
			return -5;
		}
		*(volatile uint32_t *)dm->fast = level;
		__asm__ volatile ("dsb sy" ::: "memory");
		if (dm->fast_db) {
			const uint32_t keep = *(volatile uint32_t *)dm->fast_db & (uint32_t)dm->fast_db_keep;
			*(volatile uint32_t *)dm->fast_db = keep | (uint32_t)dm->fast_db_set;
		}
		return 0;
	case 3:
		r = scmi_call(SCMI_PROTO_PERF, 0x5, in, 3, out, 4, &n);   /* limits: max = min = level */
		return r ? r : (n ? (int)out[0] : -4);
	default:
		r = scmi_call(SCMI_PROTO_PERF, 0x7, in, 2, out, 4, &n);
		if (!r && method == 1 && cpu_mbox1) {
			wr32(cpu_mbox1, PDP_MBOX_TX_CMD(0), 1u);
		}
		return r ? r : (n ? (int)out[0] : -4);
	}
}

static int
cpu_set_level(uint32_t d, uint32_t level)
{
	return cpu_set_level_via(d, level, cpu_dom[d].method < 0 ? 0 : cpu_dom[d].method);
}

static uint32_t
cpu_get(uint32_t d, uint32_t msg, uint32_t word)
{
	uint32_t in[1] = { d }, out[8], n = 0;
	return (!scmi_call(SCMI_PROTO_PERF, msg, in, 1, out, 8, &n) && n > word && (int)out[0] == 0) ? out[word] : ~0u;
}

/* The SCMI fast channel the firmware describes for DOMAIN's message MSG (0xb), mapped when it is plausible. */
static void
cpu_fast_channel(uint32_t d, uint32_t msg)
{
	struct cpu_domain *dm = &cpu_dom[d];
	uint32_t in[2] = { d, msg }, out[16], n = 0;
	const int r = scmi_call(SCMI_PROTO_PERF, 0xb, in, 2, out, 16, &n);
	printf("PANDORA CPU: domain %u fast channel for message %u: error %d, %u words:", d, msg, r, n);
	for (uint32_t i = 0; i < n && i < 12; i++) {
		printf(" %08x", out[i]);
	}
	printf("\n");
	if (r || n < 6 || out[0] != 0 || msg != 0x7) {
		return;
	}
	/* status, attributes, rate limit, chan addr low/high, chan size, doorbell addr low/high, set mask low/high,
	 * preserve mask low/high */
	const uint64_t chan = (uint64_t)out[4] << 32 | out[3], db = n > 7 ? ((uint64_t)out[7] << 32 | out[6]) : 0;
	if (chan && chan < CPU_IOS_RAM_PA && out[5] >= 4) {
		const vm_offset_t page = ml_io_map(chan & ~0xfffull, 0x1000);
		dm->fast = page ? page + (chan & 0xfff) : 0;
	}
	if (db && db < CPU_IOS_RAM_PA && (out[1] & 0x1)) {
		const vm_offset_t page = ml_io_map(db & ~0xfffull, 0x1000);
		dm->fast_db = page ? page + (db & 0xfff) : 0;
		dm->fast_db_set = n > 9 ? ((uint64_t)out[9] << 32 | out[8]) : 1;
		dm->fast_db_keep = n > 11 ? ((uint64_t)out[11] << 32 | out[10]) : 0;
	}
	printf("PANDORA CPU: domain %u fast channel at 0x%llx (%s), doorbell 0x%llx (%s)\n", d, chan, dm->fast ? "mapped" : "not used",
	    db, dm->fast_db ? "mapped" : "not used");
}

/* Set LEVEL on domain D by METHOD and see what the firmware and the clock do. */
static bool
cpu_try(uint32_t d, uint32_t level, int method, const char *how)
{
	struct cpu_domain *dm = &cpu_dom[d];
	const int r = cpu_set_level_via(d, level, method);
	IOSleep(20);
	const uint32_t fw = cpu_get(d, 0x8, 1);
	printf("PANDORA CPU: check: domain %u level %u (%u MHz) by %s: result %d, firmware level now %d, cpu %u about %u MHz "
	    "(this measure reads about 1.36 x the clock)\n", d, level, dm->khz[level] / 1000, how, r, (int)fw, dm->first,
	    cpu_clock_on((int)dm->first));
	return r == 0 && fw == level;
}

/* Does a level set move the clock? Domain 0 to its top and bottom level; domain 1 four ways, each towards its own
 * level, the first that the firmware follows kept for the governor. */
static void
cpu_check_levels(void)
{
	cpu_mbox1 = ml_io_map(PDP1_MBOX_TX_PA, 0x1000);
	(void)cpu_try(0, cpu_dom[0].top, 0, "SCMI");
	cpu_dom[0].method = cpu_try(0, 0, 0, "SCMI") ? 0 : -1;
	cpu_fast_channel(0, 0x7);
	cpu_fast_channel(1, 0x7);
	if (cpu_dom[0].method == 0 && cpu_dom[0].fast && cpu_try(0, 5, 2, "fast channel")) {
		cpu_dom[0].method = 2;
	}
	struct cpu_domain *d1 = &cpu_dom[1];
	const uint32_t back = cpu_get(1, 0x8, 1);
	if (cpu_try(1, d1->top, 0, "SCMI")) {
		d1->method = 0;
	} else if (cpu_try(1, 20, 1, "SCMI + pdp1 doorbell")) {
		d1->method = 1;
	} else if (d1->fast && cpu_try(1, 14, 2, "fast channel")) {
		d1->method = 2;
	} else if (cpu_try(1, 10, 3, "limits")) {
		d1->method = 3;
	}
	if (d1->method != 3) {   /* the limits back to the whole range */
		uint32_t in[3] = { 1, 28, 0 }, out[4], n = 0;
		(void)scmi_call(SCMI_PROTO_PERF, 0x5, in, 3, out, 4, &n);
	}
	if (d1->method < 0 && back != ~0u) {
		(void)cpu_set_level_via(1, back, 0);
	}
	printf("PANDORA CPU: check: domain 0 method %d, domain 1 method %d (0 SCMI, 1 SCMI + pdp1 doorbell, 2 fast channel, "
	    "3 limits, -1 none)\n", cpu_dom[0].method, d1->method);
}

/* ---- temperatures (patch 0205) ----
 * The SoC's temperature sensors (qcom,tsens-v2; Linux drivers/thermal/qcom/tsens-v2.c): sensor S's last reading
 * at TM + 0xa0 + 4 * S, bits 0-11 signed tenths of a degree, bit 21 valid. Which sensor is where comes from the
 * device tree's thermal zones: tsens1 cpu-0-N-M (cores 0-5, two each), tsens3 cpu-1-N-M (cores 6-7, two each),
 * tsens5 gpuss-0..10 and ddr (sensor 11). Android throttles the big cores from about 105 C (thermal HAL trip). */
static const struct {
	uint64_t tm;
	uint32_t first, count;
	const char *what;
} cpu_tsens[] = {
	{ 0xc22a000ull, 0, 12, "little" },
	{ 0xc22c000ull, 0, 4, "big" },
	{ 0xc22e000ull, 0, 11, "gpu" },
	{ 0xc22e000ull, 11, 1, "ddr" },
};
static vm_offset_t cpu_tsens_va[4];

/* The hottest valid sensor of group G in tenths of a degree (its index in *WHICH), or INT32_MIN. */
static int32_t
cpu_temp_max(uint32_t g, uint32_t *which)
{
	int32_t best = INT32_MIN;
	if (!cpu_tsens_va[g]) {
		return best;
	}
	for (uint32_t s = cpu_tsens[g].first; s < cpu_tsens[g].first + cpu_tsens[g].count; s++) {
		const uint32_t v = rd32(cpu_tsens_va[g], 0xa0 + s * 4);
		if (!(v & (1u << 21))) {
			continue;
		}
		const int32_t t = (int32_t)(v << 20) >> 20;
		if (t > best) {
			best = t;
			*which = s;
		}
	}
	return best;
}

static void
cpu_temps(bool all)
{
	char line[160];
	int len = 0;
	for (uint32_t g = 0; g < sizeof(cpu_tsens) / sizeof(cpu_tsens[0]); g++) {
		if (!cpu_tsens_va[g]) {
			cpu_tsens_va[g] = ml_io_map(cpu_tsens[g].tm, 0x1000);
		}
		if (all && cpu_tsens_va[g]) {   /* every sensor once, raw, to check the readings make sense */
			printf("PANDORA CPU: temperature %s (0x%llx):", cpu_tsens[g].what, cpu_tsens[g].tm);
			for (uint32_t s = cpu_tsens[g].first; s < cpu_tsens[g].first + cpu_tsens[g].count; s++) {
				printf(" %08x", rd32(cpu_tsens_va[g], 0xa0 + s * 4));
			}
			printf("\n");
		}
		uint32_t which = 0;
		const int32_t t = cpu_temp_max(g, &which);
		if (t == INT32_MIN) {
			len += snprintf(line + len, sizeof(line) - (size_t)len, "%s%s none", g ? ", " : "", cpu_tsens[g].what);
		} else {
			len += snprintf(line + len, sizeof(line) - (size_t)len, "%s%s %d.%d C (sensor %u)", g ? ", " : "",
			    cpu_tsens[g].what, t / 10, (t < 0 ? -t : t) % 10, which);
		}
		if (len >= (int)sizeof(line)) {
			len = (int)sizeof(line) - 1;
		}
	}
	printf("PANDORA CPU: temperature: %s\n", line);
}

/* ---- thermal control (patch 0213) ----
 * The phone's DT thermal zones, applied as Android's kernel applies them (step_wise; every 100 ms here):
 *   - cpu-N-*: thermal-hal-config 105 C, hysteresis 5 (the thermal HAL's frequency mitigation): every
 *     100 ms a domain with a core at or over 105 C gets its level cap one level under where it runs;
 *     with every core under 100 C the cap goes up one level, then away;
 *   - cpuN-emerg0-1-cfg 114 C, hysteresis 6 (cooling device cpuN-hotplug): the core leaves the scheduler,
 *     through the interface Apple's performance controller uses for that
 *     (sched_perfcontrol_update_recommended_cores, reason CLPC system), until it is under 108 C. CPU 0 stays;
 *   - cpuN-emerg0-cfg 118 C, hysteresis 8 (cpuN-pause): the domain's cap drops to its lowest level;
 *   - gpuss-N tj_cfg 95 C, no hysteresis (the kgsl cooling device, every level): the GPU's level cap
 *     (bsd/dev/pandora_gpu.c) one level lower every 100 ms at or over 95 C, one higher under it.
 * Above that the CPUCP firmware limits the clocks by itself (freq5 never went past 105 C), and 125 C
 * (reset-mon-cfg) resets the SoC. Each core has two sensors: tsens1 2N and 2N+1 for core N of 0-5,
 * tsens3 0-1 for core 6 and 2-3 for core 7.
 * Boot-arg pandora_thermal_test=N: every threshold N degrees lower, to exercise this at an ordinary load.
 * Patch 0257: under these, the body temperature's caps (pandora_cpu_body_cap, from pandora_therm.c), which
 * hold the clusters far below such junction temperatures, as Android's mi_thermald does.
 */
#define T_HAL       1050    /* tenths of a degree */
#define T_HAL_HYST  50
#define T_PLUG      1140
#define T_PLUG_HYST 60
#define T_PAUSE     1180
#define T_GPU       950
extern uint32_t pandora_gpu_thermal_top;             /* bsd/dev/pandora_gpu.c: GPU level cap, 0 = none */
extern const uint32_t pandora_gpu_level_count;
extern void pandora_cpu_recommend(uint64_t cores);  /* osfmk/arm/pandora_cpu_idle.c */
static uint32_t cpu_tcap[2] = { ~0u, ~0u };          /* level cap per domain, ~0u = none */
static uint32_t cpu_bcap[2] = { ~0u, ~0u };          /* 0257: the body temperature's level cap, ~0u = none */
static uint64_t cpu_t_out;                           /* cores taken from the scheduler for heat */
static int32_t cpu_t_test;                           /* tenths added to readings (pandora_thermal_test) */
static uint32_t cpu_t_logged;

/* The highest valid reading of sensors FIRST .. FIRST + COUNT - 1 of group G, or INT32_MIN. */
static int32_t
cpu_temp_of(uint32_t g, uint32_t first, uint32_t count)
{
	int32_t best = INT32_MIN;
	if (!cpu_tsens_va[g]) {
		return best;
	}
	for (uint32_t s = first; s < first + count; s++) {
		const uint32_t v = rd32(cpu_tsens_va[g], 0xa0 + s * 4);
		if (!(v & (1u << 21))) {
			continue;
		}
		const int32_t t = (int32_t)(v << 20) >> 20;
		best = t > best ? t : best;
	}
	return best;
}

/* 0259: for iOS's die sensors (pandora_therm.c): the highest, or with MEAN the average, valid reading of sensors
 * FIRST .. FIRST + COUNT - 1 of group G (0: cores 0-5, 1: cores 6-7, 2: GPU), in tenths of a degree; INT32_MIN
 * when none is valid. */
int32_t pandora_cpu_tsens(uint32_t g, uint32_t first, uint32_t count, int mean);
int32_t
pandora_cpu_tsens(uint32_t g, uint32_t first, uint32_t count, int mean)
{
	if (g > 2 || !cpu_tsens_va[g]) {
		return INT32_MIN;
	}
	if (!mean) {
		return cpu_temp_of(g, first, count);
	}
	int32_t sum = 0, n = 0;
	for (uint32_t s = first; s < first + count; s++) {
		const uint32_t v = rd32(cpu_tsens_va[g], 0xa0 + s * 4);
		if (v & (1u << 21)) {
			sum += (int32_t)(v << 20) >> 20;
			n++;
		}
	}
	return n ? sum / n : INT32_MIN;
}

static void
cpu_thermal(void)
{
	int32_t hot[2] = { INT32_MIN, INT32_MIN };
	uint64_t out = cpu_t_out;
	bool pause[2] = { false, false }, changed = false;
	for (int c = 0; c < 8; c++) {
		int32_t t = c < 6 ? cpu_temp_of(0, 2 * (uint32_t)c, 2) : cpu_temp_of(1, 2 * (uint32_t)(c - 6), 2);
		if (t == INT32_MIN) {
			continue;
		}
		t += cpu_t_test;
		const int d = c < 6 ? 0 : 1;
		hot[d] = t > hot[d] ? t : hot[d];
		if (c != 0 && t >= T_PLUG) {
			out |= 1ull << c;
		} else if (t < T_PLUG - T_PLUG_HYST) {
			out &= ~(1ull << c);
		}
		if (t >= T_PAUSE) {
			pause[d] = true;
		}
	}
	for (uint32_t d = 0; d < 2; d++) {
		const struct cpu_domain *dm = &cpu_dom[d];
		uint32_t cap = cpu_tcap[d];
		if (pause[d]) {
			cap = 0;
		} else if (hot[d] != INT32_MIN && hot[d] >= T_HAL) {
			const uint32_t from = cap < dm->level ? cap : dm->level;
			cap = from ? from - 1 : 0;
		} else if (hot[d] != INT32_MIN && hot[d] < T_HAL - T_HAL_HYST && cap != ~0u) {
			cap = cap + 1 >= dm->top ? ~0u : cap + 1;
		}
		if (cap != cpu_tcap[d]) {
			cpu_tcap[d] = cap;
			changed = true;
		}
	}
	if (out != cpu_t_out) {
		pandora_cpu_recommend(~out & 0xffull);
		cpu_t_out = out;
		changed = true;
	}
	int32_t g = cpu_temp_of(2, 0, 11);
	const uint32_t levels = pandora_gpu_level_count, top = __atomic_load_n(&pandora_gpu_thermal_top, __ATOMIC_RELAXED);
	uint32_t gcap = top;
	if (g != INT32_MIN && levels > 1) {
		g += cpu_t_test;
		if (g >= T_GPU) {
			const uint32_t from = top ? top : levels;
			gcap = from > 1 ? from - 1 : 1;
		} else if (top) {
			gcap = top + 1 >= levels ? 0 : top + 1;
		}
		if (gcap != top) {
			__atomic_store_n(&pandora_gpu_thermal_top, gcap, __ATOMIC_RELAXED);
			changed = true;
		}
	}
	if (changed && cpu_t_logged < 400) {
		cpu_t_logged++;
		printf("PANDORA THERMAL: hottest core little %d.%d big %d.%d C, gpu %d.%d C%s: level caps %d %d, cores off 0x%llx, "
		    "gpu cap %u\n", hot[0] / 10, (hot[0] < 0 ? -hot[0] : hot[0]) % 10, hot[1] / 10, (hot[1] < 0 ? -hot[1] : hot[1]) % 10,
		    g / 10, (g < 0 ? -g : g) % 10, cpu_t_test ? " (test offset)" : "", (int)cpu_tcap[0], (int)cpu_tcap[1], cpu_t_out,
		    gcap);
	}
}

/* 0257: the body temperature's caps (bsd/dev/pandora_therm.c, mi_thermald's SS-CPU0 and SS-CPU6), the
 * highest kHz for each domain, 0 = none; kept as the highest level at or under it, which the governor
 * holds to as it does the junction cap. */
void pandora_cpu_body_cap(uint32_t khz0, uint32_t khz6);
void
pandora_cpu_body_cap(uint32_t khz0, uint32_t khz6)
{
	const uint32_t khz[2] = { khz0, khz6 };
	for (uint32_t d = 0; d < 2; d++) {
		uint32_t cap = ~0u;
		if (khz[d]) {
			cap = 0;
			while (cap + 1 < cpu_dom[d].levels && cpu_dom[d].khz[cap + 1] <= khz[d]) {
				cap++;
			}
		}
		__atomic_store_n(&cpu_bcap[d], cap, __ATOMIC_RELAXED);
	}
}

static void
cpu_governor(void)
{
	uint64_t now, last_log, two, last_pctl, ten;
	uint64_t idle[8] = { 0 }, stamp;
	int fix;
	nanoseconds_to_absolutetime(2000 * NSEC_PER_MSEC, &two);
	nanoseconds_to_absolutetime(10000 * NSEC_PER_MSEC, &ten);
	for (uint32_t d = 0; d < 2; d++) {
		struct cpu_domain *dm = &cpu_dom[d];
		const uint32_t top = cpu_get(d, 0x3, 4);   /* sustained level */
		if (top != ~0u && top < dm->levels) {
			dm->top = top;
		}
		fix = -1;
		(void)PE_parse_boot_argn(d ? "pandora_cpu_fix1" : "pandora_cpu_fix0", &fix, sizeof(fix));
		dm->fixed = fix >= 0 && (uint32_t)fix < dm->levels ? (uint32_t)fix : ~0u;
		if (dm->method < 0) {   /* no way to set it: left where the firmware has it */
			const uint32_t now = cpu_get(d, 0x8, 1);
			dm->fixed = now != ~0u && now < dm->levels ? now : 0;
			printf("PANDORA CPU: governor: domain %u cannot be set, left at level %u\n", d, dm->fixed);
			dm->level = dm->fixed;
			continue;
		}
		dm->level = dm->fixed != ~0u ? dm->fixed : dm->top;
		const int r = cpu_set_level(d, dm->level);
		printf("PANDORA CPU: governor: domain %u cpus %u-%u, %u levels, cap %u (%u MHz)%s, start at %u: %d\n", d, dm->first,
		    dm->first + dm->count - 1, dm->levels, dm->top, dm->khz[dm->top] / 1000, dm->fixed != ~0u ? ", fixed" : "",
		    dm->level, r);
	}
	for (int c = 0; c < 8; c++) {
		idle[c] = pandora_cpu_idle_mach(c);
	}
	stamp = last_log = last_pctl = mach_absolute_time();
	cpu_temps(true);
	pandora_perfctl_log();   /* the first call only starts the interval */
	int t_test = 0;
	if (PE_parse_boot_argn("pandora_thermal_test", &t_test, sizeof(t_test)) && t_test > 0 && t_test < 80) {
		cpu_t_test = t_test * 10;
		printf("PANDORA THERMAL: test: every threshold %d C lower\n", t_test);
	}
	for (uint64_t round = 0;; round++) {
		IOSleep(16);
		now = mach_absolute_time();
		const uint64_t span = now - stamp;
		stamp = now;
		uint32_t busy_pm[8];
		for (int c = 0; c < 8; c++) {
			const uint64_t i = pandora_cpu_idle_mach(c), di = i - idle[c];
			idle[c] = i;
			busy_pm[c] = span ? (uint32_t)(di >= span ? 0 : 1000 - di * 1000 / span) : 0;
			cpu_busy_sum[c] += busy_pm[c];
		}
		if (round % 6 == 0) {
			cpu_thermal();   /* 0213: every ~100 ms */
		}
		for (uint32_t d = 0; d < 2; d++) {
			struct cpu_domain *dm = &cpu_dom[d];
			uint32_t busy = 0;
			for (uint32_t c = dm->first; c < dm->first + dm->count; c++) {
				busy = busy_pm[c] > busy ? busy_pm[c] : busy;
			}
			dm->busy_sum += busy;
			dm->khz_sum += dm->khz[dm->level];
			dm->windows++;
			if (dm->fixed != ~0u) {
				continue;
			}
			/* schedutil: 1.25 x what was used at the current clock; nearly always busy: at least twice the clock */
			uint64_t need = (uint64_t)dm->khz[dm->level] * busy * 5 / 4 / 1000;
			if (busy >= 900 && need < 2ull * dm->khz[dm->level]) {
				need = 2ull * dm->khz[dm->level];
			}
			/* the floor (the bootloader's 1.67 GHz) unless nearly idle for about 0.2 s */
			dm->quiet = busy < 100 ? dm->quiet + 1 : 0;
			const uint32_t bcap = __atomic_load_n(&cpu_bcap[d], __ATOMIC_RELAXED);
			uint32_t top = cpu_tcap[d] < dm->top ? cpu_tcap[d] : dm->top;   /* 0213: the temperature cap */
			top = bcap < top ? bcap : top;                                  /* 0257: the body temperature's */
			const uint32_t floor = dm->quiet >= 12 ? 0 : (dm->floor < top ? dm->floor : top);
			uint32_t want = floor;
			while (want < top && dm->khz[want] < need) {
				want++;
			}
			if (dm->level > top) {
				dm->down_windows = 0;   /* over the cap: down at once */
			} else if (want > dm->level) {
				dm->down_windows = 0;
			} else if (want < dm->level) {
				dm->want_max_down = dm->down_windows ? (want > dm->want_max_down ? want : dm->want_max_down) : want;
				if (++dm->down_windows < 4) {
					continue;
				}
				want = dm->want_max_down < top ? dm->want_max_down : top;
				dm->down_windows = 0;
			} else {
				dm->down_windows = 0;
				continue;
			}
			if (want != dm->level) {
				if (cpu_set_level(d, want) == 0) {
					dm->level = want;
					dm->changes++;
				} else if (dm->set_errors++ < 5) {
					printf("PANDORA CPU: governor: domain %u level %u refused\n", d, want);
				}
			}
		}
		/* every 2 s: average busyness and clock per domain, changes, the firmware's limit and level */
		if (now - last_log >= two) {
			last_log = now;
			if (round < 4000 || (round % 1250) < 125) {
				uint64_t w = cpu_dom[0].windows ? cpu_dom[0].windows : 1;
				printf("PANDORA CPU: governor: busy per cpu %llu %llu %llu %llu %llu %llu | %llu %llu %%\n",
				    cpu_busy_sum[0] / w / 10, cpu_busy_sum[1] / w / 10, cpu_busy_sum[2] / w / 10, cpu_busy_sum[3] / w / 10,
				    cpu_busy_sum[4] / w / 10, cpu_busy_sum[5] / w / 10, cpu_busy_sum[6] / w / 10, cpu_busy_sum[7] / w / 10);
				cpu_temps(false);
			}
			for (int c = 0; c < 8; c++) {
				cpu_busy_sum[c] = 0;
			}
			for (uint32_t d = 0; d < 2; d++) {
				struct cpu_domain *dm = &cpu_dom[d];
				const uint32_t limit = cpu_get(d, 0x6, 1), fw = cpu_get(d, 0x8, 1);
				const bool taken = fw != ~0u && fw < dm->levels && fw != dm->level;
				if (round < 4000 || (round % 1250) < 125) {
					printf("PANDORA CPU: governor: domain %u busy %llu%% avg %llu MHz, level %u (%u MHz), %u changes, limit %d, firmware %d%s\n",
					    d, dm->windows ? dm->busy_sum / dm->windows / 10 : 0, dm->windows ? dm->khz_sum / dm->windows / 1000 : 0,
					    dm->level, dm->khz[dm->level] / 1000, dm->changes, (int)limit, (int)fw, taken ? " (taken)" : "");
				}
				if (taken) {
					dm->level = fw;
				}
				dm->busy_sum = dm->khz_sum = dm->windows = 0;
				dm->changes = 0;
			}
		}
		if (now - last_pctl >= ten) {
			last_pctl = now;
			pandora_perfctl_log();
		}
	}
}

static void
cpu_thread(__unused void *arg, __unused wait_result_t wr)
{
	const ml_topology_info_t *t = ml_get_topology_info();
	IOSleep(3000);
	if (t) {
		printf("PANDORA CPU: %u CPUs in %u clusters (boot cpu %u)\n", t->num_cpus, t->num_clusters,
		    t->boot_cpu ? t->boot_cpu->cpu_id : 0);
		for (unsigned int i = 0; i < t->num_cpus; i++) {
			printf("PANDORA CPU: cpu %u phys 0x%x cluster %u type %d\n", t->cpus[i].cpu_id, t->cpus[i].phys_id,
			    t->cpus[i].cluster_id, (int)t->cpus[i].cluster_type);
		}
		for (unsigned int c = 0; c < t->num_clusters; c++) {
			printf("PANDORA CPU: cluster %u type %d cpus %u from %u mask 0x%llx\n", t->clusters[c].cluster_id,
			    (int)t->clusters[c].cluster_type, t->clusters[c].num_cpus, t->clusters[c].first_cpu_id,
			    t->clusters[c].cpu_mask);
		}
	}
	pandora_idle_start();
	pandora_prof_start();          /* only with boot-arg pandora_prof=NAME */
	pandora_autoswipe_start();     /* only with boot-arg pandora_autoswipe=SECONDS */
	cpu_clocks("at 3 s");
	cpu_scmi_probe();
	int mode = 1;
	(void)PE_parse_boot_argn("pandora_cpu", &mode, sizeof(mode));
	if (mode >= 2 && cpu_mbox && cpu_shmem) {
		pandora_perfctl_start();
		cpu_check_levels();
		cpu_clocks("after the check");
		cpu_governor();   /* does not return */
	}
	IOSleep(17000);
	cpu_clocks("at 20 s");
	IOSleep(40000);
	cpu_clocks("at 60 s");
}

void pandora_cpu_init(void);
void
pandora_cpu_init(void)
{
	thread_t thread;
	int on = 1;
	(void)PE_parse_boot_argn("pandora_cpu", &on, sizeof(on));
	if (on <= 0) {
		return;
	}
	if (kernel_thread_start(cpu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
