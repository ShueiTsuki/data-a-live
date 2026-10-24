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

#define PDP_MBOX_TX_PA          0x19980000ull   /* qcom,pdp0 "tx" registers */
#define PDP_MBOX_TX_CMD(ch)     (0x100u + (ch) * 8u + 4u)
#define PDP_SHMEM_PA            0x81f08000ull   /* pdp-sram: rx area at +0x000, tx area at +0x100 */
#define PDP_SHMEM_TX            0x100u
#define PDP_SHMEM_SIZE          0x80u
#define SCMI_PROTO_PERF         0x13u
#define SCMI_FREE               0x1u
#define SCMI_ERROR              0x2u

static vm_offset_t cpu_mbox, cpu_shmem;
static uint32_t cpu_token;

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
		for (int guard = 0; guard < 16; guard++) {
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
};
static struct cpu_domain cpu_dom[2] = {
	{ cpu_khz0, sizeof(cpu_khz0) / 4, 0, 6, 0, 27, ~0u, 0, 0, 0, 0, 0, 0, 0 },
	{ cpu_khz1, sizeof(cpu_khz1) / 4, 6, 2, 0, 26, ~0u, 0, 0, 0, 0, 0, 0, 0 },
};

static int
cpu_set_level(uint32_t d, uint32_t level)
{
	uint32_t in[2] = { d, level }, out[4], n = 0;
	const int r = scmi_call(SCMI_PROTO_PERF, 0x7, in, 2, out, 4, &n);
	return r ? r : (n ? (int)out[0] : -4);
}

static uint32_t
cpu_get(uint32_t d, uint32_t msg, uint32_t word)
{
	uint32_t in[1] = { d }, out[8], n = 0;
	return (!scmi_call(SCMI_PROTO_PERF, msg, in, 1, out, 8, &n) && n > word && (int)out[0] == 0) ? out[word] : ~0u;
}

/* Does a level set move the clock? Each domain to its top and bottom level, measured on its first CPU. */
static void
cpu_check_levels(void)
{
	for (uint32_t d = 0; d < 2; d++) {
		struct cpu_domain *dm = &cpu_dom[d];
		const uint32_t tries[2] = { dm->top, 0 };
		for (int k = 0; k < 2; k++) {
			const int r = cpu_set_level(d, tries[k]);
			IOSleep(20);
			printf("PANDORA CPU: check: domain %u set level %u (%u MHz): result %d, firmware level now %d, cpu %u about %u MHz\n",
			    d, tries[k], dm->khz[tries[k]] / 1000, r, (int)cpu_get(d, 0x8, 1), dm->first, cpu_clock_on((int)dm->first));
		}
	}
}

static void
cpu_governor(void)
{
	uint64_t now, last_log, two;
	uint64_t idle[8] = { 0 }, stamp;
	int fix;
	nanoseconds_to_absolutetime(2000 * NSEC_PER_MSEC, &two);
	for (uint32_t d = 0; d < 2; d++) {
		struct cpu_domain *dm = &cpu_dom[d];
		const uint32_t top = cpu_get(d, 0x3, 4);   /* sustained level */
		if (top != ~0u && top < dm->levels) {
			dm->top = top;
		}
		fix = -1;
		(void)PE_parse_boot_argn(d ? "pandora_cpu_fix1" : "pandora_cpu_fix0", &fix, sizeof(fix));
		dm->fixed = fix >= 0 && (uint32_t)fix < dm->levels ? (uint32_t)fix : ~0u;
		dm->level = dm->fixed != ~0u ? dm->fixed : dm->top;
		const int r = cpu_set_level(d, dm->level);
		printf("PANDORA CPU: governor: domain %u cpus %u-%u, %u levels, cap %u (%u MHz)%s, start at %u: %d\n", d, dm->first,
		    dm->first + dm->count - 1, dm->levels, dm->top, dm->khz[dm->top] / 1000, dm->fixed != ~0u ? ", fixed" : "",
		    dm->level, r);
	}
	for (int c = 0; c < 8; c++) {
		idle[c] = pandora_cpu_idle_mach(c);
	}
	stamp = last_log = mach_absolute_time();
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
			uint32_t want = 0;
			while (want < dm->top && dm->khz[want] < need) {
				want++;
			}
			if (want > dm->level) {
				dm->down_windows = 0;
			} else if (want < dm->level) {
				dm->want_max_down = dm->down_windows ? (want > dm->want_max_down ? want : dm->want_max_down) : want;
				if (++dm->down_windows < 4) {
					continue;
				}
				want = dm->want_max_down;
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
			for (uint32_t d = 0; d < 2; d++) {
				struct cpu_domain *dm = &cpu_dom[d];
				const uint32_t limit = cpu_get(d, 0x6, 1), fw = cpu_get(d, 0x8, 1);
				if (limit != ~0u && limit < dm->top && dm->level > limit) {
					dm->level = limit;
				}
				if (round < 4000 || (round % 1250) < 125) {
					printf("PANDORA CPU: governor: domain %u busy %llu%% avg %llu MHz, level %u (%u MHz), %u changes, limit %d, firmware %d\n",
					    d, dm->windows ? dm->busy_sum / dm->windows / 10 : 0, dm->windows ? dm->khz_sum / dm->windows / 1000 : 0,
					    dm->level, dm->khz[dm->level] / 1000, dm->changes, (int)limit, (int)fw);
				}
				dm->busy_sum = dm->khz_sum = dm->windows = 0;
				dm->changes = 0;
			}
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
	cpu_clocks("at 3 s");
	cpu_scmi_probe();
	int mode = 1;
	(void)PE_parse_boot_argn("pandora_cpu", &mode, sizeof(mode));
	if (mode >= 2 && cpu_mbox && cpu_shmem) {
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
