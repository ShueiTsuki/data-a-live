/*
 * PANDORA CPU governor helper (patch 0201): a CPU's idle time so far, for bsd/dev/pandora_cpu.c, which cannot
 * see struct processor. Mach absolute time units; recount brings it up to now when the CPU is idle.
 */
#include <kern/processor.h>
#include <kern/recount.h>
#include <kern/clock.h>
#include <kern/misc_protos.h>
#include <pexpert/pexpert.h>

uint64_t pandora_cpu_idle_mach(int cpu);
uint64_t
pandora_cpu_idle_mach(int cpu)
{
	processor_t p = cpu_to_processor(cpu);
	if (p == PROCESSOR_NULL) {
		return 0;
	}
	struct recount_usage usage = {};
	uint64_t idle = 0;
	recount_processor_usage(&p->pr_recount, &usage, &idle);
	return idle;
}

/*
 * Patch 0213: the cores the scheduler may use, as Apple's performance controller sets them for heat
 * (reason CLPC system; the boot CPU is always kept). For bsd/dev/pandora_cpu.c's thermal control.
 */
void pandora_cpu_recommend(uint64_t cores);
void
pandora_cpu_recommend(uint64_t cores)
{
	sched_perfcontrol_update_recommended_cores_reason(cores, REASON_CLPC_SYSTEM, 0);
}

/*
 * Patch 0215: the cores' idle state. The phone's DT (idle-states, entry-method "psci") gives each core one
 * state, "ret": medium-cluster0-c4 for cpu 0-5 and large-cluster1-c4 for cpu 6-7, arm,psci-suspend-param 0x4
 * (PSCI's original format, bit 16 clear: a standby state, the core keeps its context), no local-timer-stop
 * (the timer keeps running), min-residency 560 us (0x230) and 686 us (0x2ae). Linux cpuidle-psci enters it as
 * CPU_SUSPEND(0x4) with no entry point and its governors choose it when the next wake-up is at least that far
 * off. Here cpu_idle (osfmk/arm64/cpu.c) calls pandora_idle_wfi in place of WFI, as Apple's chips call
 * arm64_retention_wfi there: it takes "ret" when the CPU's next timer pop is at least min-residency away and
 * WFI otherwise; an interrupt ends either. The cluster and system states (ret-pll-off 0x1000054, apps-pc
 * 0x200c354) need PSCI's OS-initiated mode and are not used yet. Boot-arg pandora_idle=0: WFI only, as before.
 * Until pandora_idle_start runs (bsd/dev/pandora_cpu.c, 3 s after boot) every idle is WFI.
 */

#define PIDLE_CPUS      8
#define PIDLE_STATE_RET 0x4u

static uint64_t pidle_res[2];          /* min-residency in mach time: [0] cpu 0-5, [1] cpu 6-7 */
static int pidle_mode;                 /* 0 = WFI, 1 = core retention */
static int32_t pidle_error;            /* the last PSCI error */
static uint64_t pidle_ret[PIDLE_CPUS], pidle_ret_time[PIDLE_CPUS], pidle_wfi[PIDLE_CPUS], pidle_fail[PIDLE_CPUS];

/* SMCCC 1.0 as SweetCPU's CPU_ON: x0 is the signed PSCI result; x1-x17 do not survive. */
static int32_t
pidle_cpu_suspend(uint64_t state)
{
	register uint64_t x0 __asm__("x0") = 0xC4000001ULL;   /* CPU_SUSPEND, SMC64 */
	register uint64_t x1 __asm__("x1") = state;
	register uint64_t x2 __asm__("x2") = 0;               /* entry point: unused by a standby state */
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
	    "x14", "x15", "x16", "x17", "cc", "memory");
	return (int32_t)x0;
}

/*
 * 0220: what the firmware's PSCI offers, asked as Linux's psci_probe and cpuidle-psci-domain ask it, before the cluster
 * and system states (they need OS-initiated mode): the version, CPU_SUSPEND's feature flags (bit 1 OS-initiated mode,
 * bit 0 the extended state id format), and whether SET_SUSPEND_MODE and STAT_RESIDENCY exist. Nothing is changed.
 */
static int32_t
pidle_psci(uint64_t fn, uint64_t arg)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = arg;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
	    "x14", "x15", "x16", "x17", "cc", "memory");
	return (int32_t)x0;
}

static void
pidle_psci_probe(void)
{
	const uint32_t version = (uint32_t)pidle_psci(0x84000000u, 0);              /* PSCI_VERSION */
	const int32_t suspend = pidle_psci(0x8400000au, 0xc4000001u);              /* PSCI_FEATURES(CPU_SUSPEND) */
	const int32_t set_mode = pidle_psci(0x8400000au, 0x8400000fu);             /* PSCI_FEATURES(SET_SUSPEND_MODE) */
	const int32_t residency = pidle_psci(0x8400000au, 0xc4000010u);            /* PSCI_FEATURES(STAT_RESIDENCY) */
	printf("PANDORA IDLE: PSCI %u.%u; CPU_SUSPEND features %#x (OS-initiated mode %s, extended state ids %s); "
	    "SET_SUSPEND_MODE %d, STAT_RESIDENCY %d\n", version >> 16, version & 0xffff, (uint32_t)suspend,
	    suspend >= 0 && (suspend & 2) ? "yes" : "no", suspend >= 0 && (suspend & 1) ? "yes" : "no", set_mode, residency);
}

void pandora_idle_start(void);
void
pandora_idle_start(void)
{
	int mode = 1;
	pidle_psci_probe();
	PE_parse_boot_argn("pandora_idle", &mode, sizeof(mode));
	nanoseconds_to_absolutetime(560 * 1000ull, &pidle_res[0]);
	nanoseconds_to_absolutetime(686 * 1000ull, &pidle_res[1]);
	__atomic_store_n(&pidle_mode, mode == 1, __ATOMIC_RELEASE);
	printf("PANDORA IDLE: %s (DT \"ret\" 0x%x when the next timer is at least 560 us / 686 us away)\n",
	    mode == 1 ? "core retention on" : "WFI only", PIDLE_STATE_RET);
}

/* In place of WFI, interrupts masked, the CPU's next timer pop at POP (mach time). */
void pandora_idle_wfi(unsigned cpu, uint64_t pop);
void
pandora_idle_wfi(unsigned cpu, uint64_t pop)
{
	if (cpu < PIDLE_CPUS && __atomic_load_n(&pidle_mode, __ATOMIC_ACQUIRE) == 1) {
		const uint64_t now = mach_absolute_time();
		const unsigned big = ((__builtin_arm_rsr64("MPIDR_EL1") >> 16) & 0xff) != 0;   /* Aff2 1: cpu 6-7 */
		if (pop > now && pop - now >= pidle_res[big]) {
			const int32_t r = pidle_cpu_suspend(PIDLE_STATE_RET);
			if (r == 0) {
				pidle_ret[cpu]++;
				pidle_ret_time[cpu] += mach_absolute_time() - now;
				return;
			}
			pidle_error = r;
			if (++pidle_fail[cpu] >= 8) {
				__atomic_store_n(&pidle_mode, 0, __ATOMIC_RELAXED);   /* refused: WFI from now on */
			}
		}
	}
	if (cpu < PIDLE_CPUS) {
		pidle_wfi[cpu]++;
	}
	__builtin_arm_wfi();
}

/* Since the last call, per CPU: retention entries with their average length, WFIs, refusals. */
void pandora_idle_log(void);
void
pandora_idle_log(void)
{
	static uint64_t shown_ret[PIDLE_CPUS], shown_time[PIDLE_CPUS], shown_wfi[PIDLE_CPUS];
	char line[384];
	int at = 0;
	for (unsigned c = 0; c < PIDLE_CPUS && at < (int)sizeof(line); c++) {
		const uint64_t ret = pidle_ret[c] - shown_ret[c], wfi = pidle_wfi[c] - shown_wfi[c];
		uint64_t ns = 0;
		absolutetime_to_nanoseconds(pidle_ret_time[c] - shown_time[c], &ns);
		shown_ret[c] += ret;
		shown_wfi[c] += wfi;
		shown_time[c] = pidle_ret_time[c];
		at += snprintf(line + at, sizeof(line) - at, " %u: %llu x %llu us / %llu", c, ret, ret ? ns / 1000 / ret : 0, wfi);
	}
	printf("PANDORA IDLE: ret x average / wfi:%s | mode %d, refused %llu (last %d)\n", line, pidle_mode,
	    pidle_fail[0] + pidle_fail[1] + pidle_fail[2] + pidle_fail[3] + pidle_fail[4] + pidle_fail[5] + pidle_fail[6] +
	    pidle_fail[7], pidle_error);
}
