/*
 * PANDORA performance-controller signals (patch 0205), boot-arg pandora_perfctl (default 1; 0 = off). Read only:
 * it counts and logs, nothing is changed.
 *
 * On an iPhone, Apple's performance controller (CLPC) sets the clocks and which cores each thread group may use.
 * xnu tells it what runs through sched_perfcontrol_register_callbacks (osfmk/arm/machine_routines_common.c):
 * every context switch with each thread's class (idle, kernel, realtime, background, utility, non-UI, UI,
 * above UI: thread_get_perfcontrol_class, from its QoS and priority), and every work interval userland reports
 * (CoreAnimation's frames: start, finish, deadline). There is no CLPC in this kernelcache, so the slot is empty;
 * this file takes it to see those signals before a governor is built on them (bsd/dev/pandora_cpu.c logs them):
 *   - per CPU, how long each class ran;
 *   - per thread, how long it ran, with its process name: the busiest threads and processes;
 *   - per process, its work intervals: how many, how many finished after their deadline, the time used against
 *     the time allowed.
 * Time is charged at each switch to the thread going off core, for the span since the CPU's previous switch.
 *
 * Patch 0207 (freq5: the 2048-entry thread table was full after a few minutes, 2.7 M switches not
 * counted, because ended threads kept their entries; a process's threads kept the name "launchd" they
 * had before exec; with more than 64 names a busy process could miss the list): 4096 entries, an entry
 * free again after a minute without time, the name taken again at every switch, 256 process names.
 */
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/processor.h>
#include <kern/cpu_number.h>
#include <kern/clock.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern char *proc_name_address(void *p);   /* bsd/kern/kern_proc.c */
extern void pandora_idle_log(void);         /* osfmk/arm/pandora_cpu_idle.c (0215) */

#define PCTL_CPUS       8
#define PCTL_THREADS    4096               /* power of two */
#define PCTL_PROCS      32
#define PCTL_NAME       17

struct pctl_thread {
	uint64_t        id;                     /* thread id, 0 = free */
	uint64_t        time;                   /* mach time on core */
	uint64_t        shown;                  /* time at the last log (the logger's) */
	uint32_t        cls;                    /* last perfcontrol class seen */
	uint32_t        idle_logs;              /* logs in a row without time (the logger's) */
	char            name[PCTL_NAME];
};
struct pctl_frames {
	uint32_t        key;                    /* hash of the process name, 0 = free */
	char            name[PCTL_NAME];
	uint64_t        count, late, used, allowed;
	uint64_t        shown_count, shown_late, shown_used, shown_allowed;
};

static uint64_t pctl_last[PCTL_CPUS];
static uint64_t pctl_class[PCTL_CPUS][PERFCONTROL_CLASS_MAX];
static uint64_t pctl_class_shown[PCTL_CPUS][PERFCONTROL_CLASS_MAX];
static struct pctl_thread pctl_threads[PCTL_THREADS];
static struct pctl_frames pctl_frames[PCTL_PROCS];
static uint64_t pctl_dropped, pctl_switches, pctl_notifies, pctl_ctls;

static const char *
pctl_proc_name(thread_t th)
{
	task_t t = th ? get_threadtask(th) : TASK_NULL;
	void *p = t ? get_bsdtask_info(t) : NULL;
	return p ? proc_name_address(p) : "?";
}

static uint32_t
pctl_hash_name(const char *s)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < PCTL_NAME - 1 && s[i]; i++) {
		h = (h ^ (uint8_t)s[i]) * 16777619u;
	}
	return h ? h : 1;
}

/* The slot of thread ID, inserted (with its process name from TH) when new; NULL when the table is full. */
static struct pctl_thread *
pctl_thread_slot(uint64_t id, thread_t th)
{
	uint32_t i = (uint32_t)((id * 0x9e3779b97f4a7c15ull) >> 52) & (PCTL_THREADS - 1);
	for (int probe = 0; probe < 32; probe++, i = (i + 1) & (PCTL_THREADS - 1)) {
		struct pctl_thread *s = &pctl_threads[i];
		uint64_t cur = os_atomic_load(&s->id, relaxed);
		if (cur == id) {
			strlcpy(s->name, pctl_proc_name(th), sizeof(s->name));   /* again: exec renames */
			return s;
		}
		if (cur == 0 && os_atomic_cmpxchg(&s->id, 0ull, id, relaxed)) {
			strlcpy(s->name, pctl_proc_name(th), sizeof(s->name));
			return s;
		}
		if (os_atomic_load(&s->id, relaxed) == id) {
			strlcpy(s->name, pctl_proc_name(th), sizeof(s->name));
			return s;
		}
	}
	os_atomic_inc(&pctl_dropped, relaxed);
	return NULL;
}

static void
pctl_csw(perfcontrol_event event, uint32_t cpu, uint64_t timestamp, __unused uint32_t flags,
    struct perfcontrol_thread_data *offcore, __unused struct perfcontrol_thread_data *oncore,
    __unused struct perfcontrol_cpu_counters *counters, __unused void *unused)
{
	(void)event;
	if (cpu >= PCTL_CPUS || offcore == NULL) {
		return;
	}
	const uint64_t last = pctl_last[cpu];
	pctl_last[cpu] = timestamp;
	os_atomic_inc(&pctl_switches, relaxed);
	if (last == 0 || timestamp <= last) {
		return;
	}
	const uint64_t span = timestamp - last;
	const uint32_t cls = offcore->perfctl_class < PERFCONTROL_CLASS_MAX ? offcore->perfctl_class : 0;
	pctl_class[cpu][cls] += span;   /* this CPU's own counters, written only here */
	if (cls == PERFCONTROL_CLASS_IDLE || offcore->perfctl_state == NULL) {
		return;
	}
	thread_t th = __container_of((struct perfcontrol_state *)offcore->perfctl_state, struct thread, machine.perfctrl_state);
	struct pctl_thread *s = pctl_thread_slot(offcore->thread_id, th);
	if (s != NULL) {
		os_atomic_add(&s->time, span, relaxed);
		s->cls = cls;
	}
}

static void
pctl_frame(uint64_t start, uint64_t finish, uint64_t deadline)
{
	if (start == 0 || finish <= start || deadline <= start) {
		return;
	}
	const char *name = pctl_proc_name(current_thread());
	const uint32_t key = pctl_hash_name(name);
	for (uint32_t i = 0; i < PCTL_PROCS; i++) {
		struct pctl_frames *f = &pctl_frames[(key + i) % PCTL_PROCS];
		uint32_t cur = os_atomic_load(&f->key, relaxed);
		if (cur == 0 && os_atomic_cmpxchg(&f->key, 0u, key, relaxed)) {
			strlcpy(f->name, name, sizeof(f->name));
			cur = key;
		} else if (cur == 0) {
			cur = os_atomic_load(&f->key, relaxed);
		}
		if (cur == key) {
			os_atomic_inc(&f->count, relaxed);
			if (finish > deadline) {
				os_atomic_inc(&f->late, relaxed);
			}
			os_atomic_add(&f->used, finish - start, relaxed);
			os_atomic_add(&f->allowed, deadline - start, relaxed);
			return;
		}
	}
}

static void
pctl_work_interval_notify(__unused perfcontrol_state_t state, perfcontrol_work_interval_t wi)
{
	os_atomic_inc(&pctl_notifies, relaxed);
	pctl_frame(wi->start, wi->finish, wi->deadline);
}

static void
pctl_work_interval_ctl(__unused perfcontrol_state_t state, perfcontrol_work_interval_instance_t wi)
{
	os_atomic_inc(&pctl_ctls, relaxed);
	if (wi->ctl == WORK_INTERVAL_FINISH) {
		pctl_frame(wi->start, wi->finish, wi->deadline);
	}
}

static struct sched_perfcontrol_callbacks pctl_callbacks = {
	.version = SCHED_PERFCONTROL_CALLBACKS_VERSION_8,   /* up to work_interval_ctl; 9 would hand us shared-resource flags */
	.work_interval_notify = pctl_work_interval_notify,
	.csw = pctl_csw,
	.work_interval_ctl = pctl_work_interval_ctl,
};

void pandora_perfctl_start(void);
void
pandora_perfctl_start(void)
{
	int on = 1;
	(void)PE_parse_boot_argn("pandora_perfctl", &on, sizeof(on));
	if (!on) {
		return;
	}
	sched_perfcontrol_register_callbacks(&pctl_callbacks, 0);
	printf("PANDORA PCTL: registered as the performance controller (counting only)\n");
}

static const char *const pctl_class_names[PERFCONTROL_CLASS_MAX] = {
	"?", "idle", "kernel", "rt", "bg", "utility", "nonui", "ui", "aboveui",
};

/* What ran since the last call: per CPU by class, the busiest threads and processes, the frames. */
void pandora_perfctl_log(void);
void
pandora_perfctl_log(void)
{
	static struct {
		char name[PCTL_NAME];
		uint64_t time;
	} procs[256];
	struct {
		uint32_t slot;
		uint64_t time;
	} top[8] = {};
	static uint64_t last_log;
	const uint64_t now_abs = mach_absolute_time();
	uint64_t span = last_log ? now_abs - last_log : 0;   /* busyness below is in % of one CPU over this span */
	last_log = now_abs;
	if (span == 0) {
		return;   /* the first call starts the interval */
	}

	for (uint32_t c = 0; c < PCTL_CPUS; c++) {
		uint64_t d[PERFCONTROL_CLASS_MAX], sum = 0;
		for (uint32_t k = 0; k < PERFCONTROL_CLASS_MAX; k++) {
			const uint64_t now = pctl_class[c][k];
			d[k] = now - pctl_class_shown[c][k];
			pctl_class_shown[c][k] = now;
			sum += d[k];
		}
		if (sum == 0) {
			printf("PANDORA PCTL: cpu %u: no switches\n", c);
			continue;
		}
		printf("PANDORA PCTL: cpu %u %%: idle %llu kernel %llu rt %llu bg %llu utility %llu nonui %llu ui %llu aboveui %llu\n", c,
		    d[1] * 100 / sum, d[2] * 100 / sum, d[3] * 100 / sum, d[4] * 100 / sum, d[5] * 100 / sum, d[6] * 100 / sum,
		    d[7] * 100 / sum, d[8] * 100 / sum);
	}

	uint32_t nprocs = 0;
	bzero(procs, sizeof(procs));
	for (uint32_t i = 0; i < PCTL_THREADS; i++) {
		struct pctl_thread *s = &pctl_threads[i];
		if (os_atomic_load(&s->id, relaxed) == 0) {
			continue;
		}
		const uint64_t now = os_atomic_load(&s->time, relaxed), d = now - s->shown;
		s->shown = now;
		if (d == 0) {
			if (++s->idle_logs >= 6) {   /* a minute without time: most likely ended; the entry is free again */
				s->idle_logs = 0;
				s->shown = 0;
				os_atomic_store(&s->time, 0ull, relaxed);
				os_atomic_store(&s->id, 0ull, release);
			}
			continue;
		}
		s->idle_logs = 0;
		for (int k = 0; k < 8; k++) {
			if (d > top[k].time) {
				memmove(&top[k + 1], &top[k], (size_t)(7 - k) * sizeof(top[0]));
				top[k].slot = i;
				top[k].time = d;
				break;
			}
		}
		uint32_t p = 0;
		while (p < nprocs && strncmp(procs[p].name, s->name, PCTL_NAME) != 0) {
			p++;
		}
		if (p == nprocs && nprocs < 256) {
			strlcpy(procs[p].name, s->name, PCTL_NAME);
			nprocs++;
		}
		if (p < nprocs) {
			procs[p].time += d;
		}
	}
	if (span == 0) {
		span = 1;
	}
	for (int k = 0; k < 8 && top[k].time; k++) {
		struct pctl_thread *s = &pctl_threads[top[k].slot];
		printf("PANDORA PCTL: busy thread %d: %s thread %llu (%s) %llu.%llu%% of one CPU\n", k + 1, s->name, s->id,
		    pctl_class_names[s->cls < PERFCONTROL_CLASS_MAX ? s->cls : 0], top[k].time * 100 / span,
		    top[k].time * 1000 / span % 10);
	}
	for (int k = 0; k < 8; k++) {
		uint32_t best = nprocs;
		for (uint32_t p = 0; p < nprocs; p++) {
			if (procs[p].time && (best == nprocs || procs[p].time > procs[best].time)) {
				best = p;
			}
		}
		if (best == nprocs) {
			break;
		}
		printf("PANDORA PCTL: busy process %d: %s %llu.%llu%% of one CPU\n", k + 1, procs[best].name,
		    procs[best].time * 100 / span, procs[best].time * 1000 / span % 10);
		procs[best].time = 0;
	}
	for (uint32_t i = 0; i < PCTL_PROCS; i++) {
		struct pctl_frames *f = &pctl_frames[i];
		if (os_atomic_load(&f->key, relaxed) == 0) {
			continue;
		}
		const uint64_t n = f->count - f->shown_count, late = f->late - f->shown_late;
		const uint64_t used = f->used - f->shown_used, allowed = f->allowed - f->shown_allowed;
		f->shown_count = f->count;
		f->shown_late = f->late;
		f->shown_used = f->used;
		f->shown_allowed = f->allowed;
		if (n == 0) {
			continue;
		}
		uint64_t used_us, allowed_us;
		absolutetime_to_nanoseconds(used / n, &used_us);
		absolutetime_to_nanoseconds(allowed / n, &allowed_us);
		printf("PANDORA PCTL: frames %s: %llu, %llu late, on average %llu us used of %llu us allowed\n", f->name, n, late,
		    used_us / 1000, allowed_us / 1000);
	}
	printf("PANDORA PCTL: %llu switches, %llu work interval notes, %llu instance calls, %llu threads not counted (table full)\n",
	    pctl_switches, pctl_notifies, pctl_ctls, pctl_dropped);
	pandora_idle_log();
}
