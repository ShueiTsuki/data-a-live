/*
 * PANDORA sampling profiler (patch 0222, call stacks 0223): where one process's threads are, about 1000 times a second,
 * as Apple's kperf samples on its timers. Boot-arg pandora_prof=NAME (the process, e.g. backboardd; off without it),
 * pandora_prof_hz (default 1000), pandora_prof_secs (report period, default 10).
 *
 * A kernel thread wakes at that rate and sends an interrupt (cpu_xcall) to each other CPU running a thread of the task.
 * The handler, in that CPU's interrupt context, takes the interrupted state: the user PC when the thread ran user
 * code; else the kernel PC, LR and the return addresses of up to 5 kernel frame records (inside the thread's kernel
 * stack). It also counts the sample for the thread and sets the thread's
 * kperf AST, as kperf's samplers do: before the thread returns to user mode, ast_taken_user calls pandora_prof_ast in
 * the thread's own context, which may fault, and that walks the user frame records (copyin) for the user call stack,
 * up to 16 frames, weighted by the samples counted since (one, taken in user code; or those taken in the kernel
 * before it returned).
 *
 * Each period the log gets every kernel chain ("PANDORA PROF KC <n> <pc> <lr> <return>..."), every user call stack
 * ("PANDORA PROF S <user n> <kernel n> <lr> <pc> <return>...", hex; lr is the leaf's return when the first record
 * does not hold it, else 0), and dyld's all-image info (each image outside
 * the shared cache with its load address and path, the shared cache's slide, the kernel's slide), so tools/prof.py
 * can name them and sum the stacks by function (own and inclusive time, callers). The PCs alone (0222's U/E/K lines)
 * are in these: a user sample's stack starts at its PC, a kernel sample's at the user PC it entered from (the thread
 * returns there), its chain at its kernel PC; the log keeps only the last 4 MiB (sweet_log_ring), so not twice.
 */
#include <kern/processor.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/misc_protos.h>
#include <kern/sched_prim.h>
#include <kern/kalloc.h>
#include <arm/cpu_data_internal.h>
#include <arm/machine_routines.h>
#include <mach/arm/thread_status.h>
#include <arm64/proc_reg.h>
#include <kdp/kdp_dyld.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern char *proc_name_address(void *p);
extern lck_mtx_t tasks_threads_lock;            /* kern/processor.c */
extern vm_offset_t vm_kernel_slide;
#define PPROF_NAME      16               /* MAXCOMLEN */

#define PPROF_CPUS      8
#define PPROF_RING      2048u            /* samples per CPU between two drains (1 s at 1000 Hz) */
#define PPROF_KDEPTH    6u               /* kernel LR and return addresses of a kernel sample */
#define PPROF_CHAINS    4096u            /* distinct kernel chains per period */
#define PPROF_UDEPTH    16u              /* user PC and return addresses of a call stack */
#define PPROF_STACKS    8192u            /* distinct user call stacks per period */
#define PPROF_SRING     4096u            /* call stacks between two drains */
#define PPROF_SLINES    6000u            /* call stacks logged per period */
#define PPROF_KLINES    3000u            /* kernel chains logged per period */
#define PPROF_PEND      512u             /* threads with samples awaiting their call stack */
#define PPROF_PROBE     8u

struct pprof_sample {
	uint64_t pc;            /* user PC, or kernel PC when !user */
	uint64_t kc[PPROF_KDEPTH];   /* in the kernel: LR, then the frame records' returns, 0 past the last */
	bool user;
};

static struct pprof_ring {
	volatile uint32_t head;         /* written by the CPU's handler */
	uint32_t tail;                  /* read by the profiler thread */
	struct pprof_sample s[PPROF_RING];
} *pprof_rings;
static task_t pprof_task;
static char pprof_name[PPROF_NAME + 1];
static uint64_t pprof_dropped, pprof_sent, pprof_refused, pprof_stacks_dropped, pprof_stacks_taken, pprof_unlogged;
static int pprof_param = 1;              /* cpu_xcall wants a parameter */

/* A thread's samples since its last call stack: set in the interrupt handler (the CPU the thread runs on), taken in
 * the thread's AST with interrupts off, so the two never interleave for one thread. */
static struct pprof_pend {
	thread_t th;
	uint32_t user, kernel;
} pprof_pend[PPROF_PEND];

static uint64_t pprof_user, pprof_kernel;       /* samples this period */

struct pprof_chain {
	uint64_t f[1 + PPROF_KDEPTH];
	uint32_t n;
};
static struct pprof_chain *pprof_chains;

struct pprof_stack {
	uint64_t lr;
	uint64_t f[PPROF_UDEPTH];
	uint32_t depth;
	uint32_t user, kernel;
};
static struct pprof_stack *pprof_stack_table;   /* aggregated, the profiler thread's */
static struct pprof_stack *pprof_sring;         /* taken in ASTs, under pprof_slock */
static uint32_t pprof_shead, pprof_stail;
static LCK_GRP_DECLARE(pprof_lck_grp, "pandora_prof");
static LCK_SPIN_DECLARE(pprof_slock, &pprof_lck_grp);

static uint32_t
pprof_thread_hash(thread_t th)
{
	return (uint32_t)(((uintptr_t)th >> 4) * 2654435761u);
}

/* In the CPU's interrupt context (the profiler's cpu_xcall). */
static void
pprof_sample_cpu(void *arg)
{
	(void)arg;
	thread_t th = current_thread();
	if (th == THREAD_NULL || get_threadtask(th) != pprof_task) {
		return;
	}
	cpu_data_t *cd = getCpuDatap();
	const arm_saved_state_t *st = (const arm_saved_state_t *)cd->cpu_int_state;
	if (st == NULL || cd->cpu_number >= PPROF_CPUS) {
		return;
	}
	struct pprof_ring *r = &pprof_rings[cd->cpu_number];
	const uint32_t head = r->head;
	if (head - r->tail >= PPROF_RING) {
		pprof_dropped++;
		return;
	}
	struct pprof_sample *s = &r->s[head % PPROF_RING];
	const bool user = PSR64_IS_USER(get_saved_state_cpsr(st));
	s->user = user;
	s->pc = get_saved_state_pc(st);
	bzero(s->kc, sizeof(s->kc));
	if (!user) {
		s->kc[0] = get_saved_state_lr(st);
		/* The kernel frame records, inside the thread's kernel stack only. */
		const uint64_t lo = th->kernel_stack, hi = lo + kernel_stack_size;
		uint64_t fp = get_saved_state_fp(st);
		for (unsigned k = 1; k < PPROF_KDEPTH && lo && fp >= lo && fp + 16 <= hi && (fp & 7) == 0; k++) {
			const uint64_t *rec = (const uint64_t *)fp;
			s->kc[k] = rec[1];
			if (rec[0] <= fp) {
				break;
			}
			fp = rec[0];
		}
	}
	__atomic_store_n(&r->head, head + 1, __ATOMIC_RELEASE);

	/* The user call stack, in the thread's context before it returns to user mode (pandora_prof_ast). */
	const uint32_t h = pprof_thread_hash(th);
	for (uint32_t i = 0; i < PPROF_PROBE; i++) {
		struct pprof_pend *p = &pprof_pend[(h + i) % PPROF_PEND];
		thread_t none = THREAD_NULL;
		if (p->th == th || __atomic_compare_exchange_n(&p->th, &none, th, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			if (user) {
				p->user++;
			} else {
				p->kernel++;
			}
			act_set_kperf(th);
			return;
		}
	}
	pprof_stacks_dropped++;
}

/* osfmk/kern/ast.c, AST_KPERF: in the thread's context, interrupts on, before it returns to user mode. */
void pandora_prof_ast(thread_t th);
void
pandora_prof_ast(thread_t th)
{
	if (pprof_task == TASK_NULL || get_threadtask(th) != pprof_task) {
		return;
	}
	const arm_saved_state_t *u = get_user_regs(th);
	if (u == NULL) {
		return;
	}
	struct pprof_stack s;
	bzero(&s, sizeof(s));
	s.f[s.depth++] = get_saved_state_pc(u);
	const uint64_t lr = get_saved_state_lr(u);
	uint64_t fp = get_saved_state_fp(u);
	for (bool first = true; s.depth < PPROF_UDEPTH && fp != 0 && (fp & 7) == 0; first = false) {
		uint64_t rec[2];
		if (copyin((user_addr_t)fp, rec, sizeof(rec)) != 0 || rec[1] == 0) {
			break;
		}
		/* A leaf that keeps no frame record returns to LR, which the first record does not hold (tools/prof.py
		 * drops it when it is in the leaf's own function: a function past its first call). */
		if (first && lr != rec[1]) {
			s.lr = lr;
		}
		s.f[s.depth++] = rec[1];
		if (rec[0] <= fp) {
			break;
		}
		fp = rec[0];
	}
	if (s.depth == 1) {
		s.lr = lr;
	}

	/* This thread's samples since its last call stack (in the slots its interrupts could have used). */
	const uint32_t h = pprof_thread_hash(th);
	const boolean_t ie = ml_set_interrupts_enabled(FALSE);
	for (uint32_t i = 0; i < PPROF_PROBE; i++) {
		struct pprof_pend *p = &pprof_pend[(h + i) % PPROF_PEND];
		if (p->th == th) {
			s.user += p->user;
			s.kernel += p->kernel;
			p->user = p->kernel = 0;
			__atomic_store_n(&p->th, THREAD_NULL, __ATOMIC_RELEASE);
		}
	}
	ml_set_interrupts_enabled(ie);
	if (s.user + s.kernel == 0) {
		return;
	}
	lck_spin_lock(&pprof_slock);
	if (pprof_shead - pprof_stail < PPROF_SRING) {
		pprof_sring[pprof_shead++ % PPROF_SRING] = s;
		pprof_stacks_taken++;
	} else {
		pprof_stacks_dropped++;
	}
	lck_spin_unlock(&pprof_slock);
}

static uint32_t
pprof_hash(const uint64_t *f, unsigned n)
{
	uint64_t h = 1469598103934665603ull;
	for (unsigned i = 0; i < n; i++) {
		h = (h ^ f[i]) * 1099511628211ull;
	}
	return (uint32_t)(h ^ (h >> 32));
}

static void
pprof_count_chain(const struct pprof_sample *s)
{
	uint64_t f[1 + PPROF_KDEPTH];
	f[0] = s->pc;
	memcpy(&f[1], s->kc, sizeof(s->kc));
	for (uint32_t i = 0, h = pprof_hash(f, 1 + PPROF_KDEPTH); i < PPROF_CHAINS; i++, h++) {
		struct pprof_chain *c = &pprof_chains[h % PPROF_CHAINS];
		if (c->n == 0 || memcmp(c->f, f, sizeof(f)) == 0) {
			memcpy(c->f, f, sizeof(f));
			c->n++;
			return;
		}
	}
}

static void
pprof_count_stack(const struct pprof_stack *s)
{
	for (uint32_t i = 0, h = pprof_hash(s->f, s->depth) ^ (uint32_t)s->lr; i < PPROF_STACKS; i++, h++) {
		struct pprof_stack *c = &pprof_stack_table[h % PPROF_STACKS];
		if (c->depth == 0) {
			*c = *s;
			return;
		}
		if (c->depth == s->depth && c->lr == s->lr && memcmp(c->f, s->f, s->depth * sizeof(uint64_t)) == 0) {
			c->user += s->user;
			c->kernel += s->kernel;
			return;
		}
	}
	pprof_stacks_dropped++;
}

static void
pprof_drain(void)
{
	for (unsigned cpu = 0; cpu < PPROF_CPUS; cpu++) {
		struct pprof_ring *r = &pprof_rings[cpu];
		const uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
		for (; r->tail != head; r->tail++) {
			const struct pprof_sample *s = &r->s[r->tail % PPROF_RING];
			if (s->user) {
				pprof_user++;
			} else {
				pprof_kernel++;
				pprof_count_chain(s);
			}
		}
	}
	for (;;) {
		struct pprof_stack s;
		lck_spin_lock(&pprof_slock);
		const bool any = pprof_stail != pprof_shead;
		if (any) {
			s = pprof_sring[pprof_stail++ % PPROF_SRING];
		}
		lck_spin_unlock(&pprof_slock);
		if (!any) {
			break;
		}
		pprof_count_stack(&s);
	}
}

static void
pprof_report(unsigned secs)
{
	printf("PANDORA PROF: %u s of %s: %llu in user code, %llu in the kernel (%llu interrupts sent, %llu refused, "
	    "%llu samples dropped; %llu call stacks taken, %llu dropped, %llu lines not logged)\n", secs, pprof_name,
	    pprof_user, pprof_kernel, pprof_sent, pprof_refused, pprof_dropped, pprof_stacks_taken, pprof_stacks_dropped,
	    pprof_unlogged);
	pprof_user = pprof_kernel = 0;
	/* Every kernel chain and call stack counted (a hot function's samples spread over its instructions; tools/prof.py
	 * sums them by function), at most PPROF_KLINES and PPROF_SLINES. */
	char line[(PPROF_UDEPTH + 4) * 17 + 32];
	unsigned lines = 0;
	for (uint32_t i = 0; i < PPROF_CHAINS; i++) {
		const struct pprof_chain *c = &pprof_chains[i];
		if (c->n == 0) {
			continue;
		}
		if (lines++ >= PPROF_KLINES) {
			pprof_unlogged++;
			continue;
		}
		int at = snprintf(line, sizeof(line), "PANDORA PROF KC %u", c->n);
		for (uint32_t k = 0; k < 1 + PPROF_KDEPTH && at > 0 && at < (int)sizeof(line); k++) {
			at += snprintf(line + at, sizeof(line) - at, " %llx", c->f[k]);
		}
		printf("%s\n", line);
	}
	bzero(pprof_chains, PPROF_CHAINS * sizeof(*pprof_chains));
	lines = 0;
	for (uint32_t i = 0; i < PPROF_STACKS; i++) {
		const struct pprof_stack *c = &pprof_stack_table[i];
		if (c->depth == 0) {
			continue;
		}
		if (lines++ >= PPROF_SLINES) {
			pprof_unlogged++;
			continue;
		}
		int at = snprintf(line, sizeof(line), "PANDORA PROF S %u %u %llx", c->user, c->kernel, c->lr);
		for (uint32_t k = 0; k < c->depth && at > 0 && at < (int)sizeof(line); k++) {
			at += snprintf(line + at, sizeof(line) - at, " %llx", c->f[k]);
		}
		printf("%s\n", line);
	}
	bzero(pprof_stack_table, PPROF_STACKS * sizeof(*pprof_stack_table));
}

/* The task's images outside the shared cache (load address, path), from dyld's all-image info, with every report:
 * the log keeps only its last 4 MiB, and r2l lost the one list a busy run logged first. */
static void
pprof_images(task_t task)
{
	struct user64_dyld_all_image_infos infos;
	vm_map_t map = get_task_map_reference(task);
	if (map == VM_MAP_NULL) {
		return;
	}
	if (task->all_image_info_addr == 0 ||
	    vm_map_read_user(map, task->all_image_info_addr, &infos, sizeof(infos)) != KERN_SUCCESS) {
		vm_map_deallocate(map);
		return;
	}
	const uint32_t count = infos.infoArrayCount;
	if (infos.infoArray != 0) {
		printf("PANDORA PROF IMAGES %u, shared cache base %#llx slide %#llx, kernel slide %#lx\n", count,
		    infos.sharedCacheBaseAddress, infos.sharedCacheSlide, (unsigned long)vm_kernel_slide);
		const uint64_t cache = infos.sharedCacheBaseAddress;
		for (uint32_t i = 0; i < count && i < 2048; i++) {
			struct user64_dyld_image_info img;
			char path[200];
			if (vm_map_read_user(map, infos.infoArray + i * sizeof(img), &img, sizeof(img)) != KERN_SUCCESS) {
				break;
			}
			if (cache && img.imageLoadAddress >= cache && img.imageLoadAddress < cache + (8ULL << 30)) {
				continue;
			}
			bzero(path, sizeof(path));
			(void)vm_map_read_user(map, img.imageFilePath, path, sizeof(path) - 1);
			printf("PANDORA PROF IMAGE %#llx %s\n", img.imageLoadAddress, path);
		}
	}
	vm_map_deallocate(map);
}

static task_t
pprof_find(void)
{
	task_t found = TASK_NULL, task;
	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		void *p = get_bsdtask_info(task);
		if (p != NULL && strncmp(proc_name_address(p), pprof_name, PPROF_NAME) == 0) {
			task_reference(task);
			found = task;
			break;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	return found;
}

static void
pprof_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t hz = 1000, secs = 10;
	PE_parse_boot_argn("pandora_prof_hz", &hz, sizeof(hz));
	PE_parse_boot_argn("pandora_prof_secs", &secs, sizeof(secs));
	hz = hz < 10 ? 10 : (hz > 4000 ? 4000 : hz);
	const uint32_t period_us = 1000000 / hz, per_drain = hz / 4, per_report = hz * secs;
	printf("PANDORA PROF: sampling %s at %u Hz, reports every %u s\n", pprof_name, hz, secs);
	for (uint64_t tick = 1;; tick++) {
		delay(period_us);
		if (pprof_task == TASK_NULL || !pprof_task->active) {
			if (tick % hz == 0) {
				if (pprof_task != TASK_NULL) {
					task_t old = pprof_task;
					pprof_task = TASK_NULL;
					task_deallocate(old);
				}
				pprof_task = pprof_find();
			}
			continue;
		}
		const int self = cpu_number();
		for (int cpu = 0; cpu < PPROF_CPUS && cpu <= ml_get_max_cpu_number(); cpu++) {
			cpu_data_t *cd = cpu_datap(cpu);
			thread_t t = cd != NULL ? cd->cpu_active_thread : THREAD_NULL;
			if (cpu == self || t == THREAD_NULL || get_threadtask(t) != pprof_task) {
				continue;
			}
			if (cpu_xcall(cpu, pprof_sample_cpu, &pprof_param) == KERN_SUCCESS) {
				pprof_sent++;
			} else {
				pprof_refused++;
			}
		}
		if (tick % per_drain == 0) {
			pprof_drain();
		}
		if (tick % per_report == 0) {
			pprof_images(pprof_task);
			pprof_report(secs);
		}
	}
}

/*
 * 0313: where every thread of a process stands, the user call stack of each, for a process that hangs. Boot-arg
 * pandora_stacks=NAME[,NAME...] (up to 4 processes), pandora_stacks_at (seconds after boot, default 60),
 * pandora_stacks_every (seconds between snapshots, default 0: one). The sampler above takes only threads on a CPU, so
 * a queue stuck in a wait never shows there; a thread blocked in the kernel keeps its user registers (get_user_regs),
 * and its frame records are read from the task's map. Per snapshot: the task's images (pprof_images, for the
 * addresses), then per thread "PANDORA STACKS NAME[pid] tid T st S hint H pc P lr L | returns..." (T as the unified
 * log's thread_id; S the thread's state bits, H its block_hint: what it last blocked on). On user code that runs on a
 * CPU at that moment the registers are its last kernel entry's.
 */
#define PSTACKS_NAMES   4
#define PSTACKS_THREADS 256
#define PSTACKS_DEPTH   24

static char pstacks_names[PSTACKS_NAMES][PPROF_NAME + 1];
static unsigned pstacks_count;
static thread_t pstacks_threads[PSTACKS_THREADS];       /* the snapshot thread's only */

static task_t
pstacks_find(const char *name)
{
	task_t found = TASK_NULL, task;
	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		void *p = get_bsdtask_info(task);
		if (p != NULL && task->active && strncmp(proc_name_address(p), name, PPROF_NAME) == 0) {
			task_reference(task);
			found = task;
			break;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	return found;
}

static void
pstacks_dump(task_t task, const char *name)
{
	unsigned count = 0;
	thread_t th;
	task_lock(task);
	queue_iterate(&task->threads, th, thread_t, task_threads) {
		if (count == PSTACKS_THREADS) {
			break;
		}
		thread_reference(th);
		pstacks_threads[count++] = th;
	}
	task_unlock(task);
	vm_map_t map = get_task_map_reference(task);
	const int pid = task_pid(task);
	printf("PANDORA STACKS %s[%d]: %u threads, main binary at %#llx\n", name, pid, count,
	    (unsigned long long)task->mach_header_vm_address);
	pprof_images(task);
	for (unsigned i = 0; i < count; i++) {
		th = pstacks_threads[i];
		const arm_saved_state_t *u = get_user_regs(th);
		uint64_t pc = 0, lr = 0, fp = 0;
		if (u != NULL && is_saved_state64(u)) {
			pc = get_saved_state_pc(u);
			lr = get_saved_state_lr(u);
			fp = get_saved_state_fp(u);
		}
		char line[480];
		int n = snprintf(line, sizeof(line), "PANDORA STACKS %s[%d] tid %llu st %x hint %u pc %#llx lr %#llx |", name, pid,
		    thread_tid(th), th->state, (unsigned)th->block_hint, pc, lr);
		for (unsigned d = 0; map != VM_MAP_NULL && d < PSTACKS_DEPTH && fp != 0 && (fp & 7) == 0 &&
		    n < (int)sizeof(line) - 20; d++) {
			uint64_t rec[2];
			if (vm_map_read_user(map, fp, rec, sizeof(rec)) != KERN_SUCCESS || rec[1] == 0) {
				break;
			}
			n += snprintf(line + n, sizeof(line) - n, " %llx", rec[1]);
			if (rec[0] <= fp) {
				break;
			}
			fp = rec[0];
		}
		printf("%s\n", line);
		thread_deallocate(th);
	}
	if (map != VM_MAP_NULL) {
		vm_map_deallocate(map);
	}
}

static void
pstacks_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t at = 60, every = 0;
	PE_parse_boot_argn("pandora_stacks_at", &at, sizeof(at));
	PE_parse_boot_argn("pandora_stacks_every", &every, sizeof(every));
	for (;;) {
		uint64_t now, ns;
		clock_get_uptime(&now);
		absolutetime_to_nanoseconds(now, &ns);
		if (ns / NSEC_PER_SEC >= at) {
			break;
		}
		delay(500000);
	}
	for (;;) {
		for (unsigned i = 0; i < pstacks_count; i++) {
			task_t task = pstacks_find(pstacks_names[i]);
			if (task == TASK_NULL) {
				printf("PANDORA STACKS %s: not running\n", pstacks_names[i]);
				continue;
			}
			pstacks_dump(task, pstacks_names[i]);
			task_deallocate(task);
		}
		if (every == 0) {
			return;
		}
		for (uint32_t s = 0; s < every; s++) {
			delay(1000000);
		}
	}
}

static void
pstacks_start(void)
{
	char list[PSTACKS_NAMES * (PPROF_NAME + 1)];
	if (!PE_parse_boot_arg_str("pandora_stacks", list, sizeof(list)) || list[0] == 0) {
		return;
	}
	for (char *at = list; *at != 0 && pstacks_count < PSTACKS_NAMES;) {
		size_t len = 0;
		while (at[len] != 0 && at[len] != ',') {
			len++;
		}
		strlcpy(pstacks_names[pstacks_count], at, (len < PPROF_NAME ? len : PPROF_NAME) + 1);
		if (pstacks_names[pstacks_count][0] != 0) {
			pstacks_count++;
		}
		at += len;
		if (*at == ',') {
			at++;
		}
	}
	thread_t thread;
	if (pstacks_count != 0 && kernel_thread_start_priority(pstacks_thread, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void pandora_prof_start(void);
void
pandora_prof_start(void)
{
	pstacks_start();
	if (!PE_parse_boot_arg_str("pandora_prof", pprof_name, sizeof(pprof_name)) || pprof_name[0] == 0) {
		return;
	}
	pprof_rings = kalloc_data(PPROF_CPUS * sizeof(*pprof_rings), Z_WAITOK | Z_ZERO);
	pprof_chains = kalloc_data(PPROF_CHAINS * sizeof(struct pprof_chain), Z_WAITOK | Z_ZERO);
	pprof_stack_table = kalloc_data(PPROF_STACKS * sizeof(struct pprof_stack), Z_WAITOK | Z_ZERO);
	pprof_sring = kalloc_data(PPROF_SRING * sizeof(struct pprof_stack), Z_WAITOK | Z_ZERO);
	if (pprof_rings == NULL || !pprof_chains || !pprof_stack_table || !pprof_sring) {
		printf("PANDORA PROF: no memory\n");
		return;
	}
	thread_t thread;
	if (kernel_thread_start_priority(pprof_thread, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
