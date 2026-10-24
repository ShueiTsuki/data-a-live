/*
 * PANDORA CPU governor helper (patch 0201): a CPU's idle time so far, for bsd/dev/pandora_cpu.c, which cannot
 * see struct processor. Mach absolute time units; recount brings it up to now when the CPU is idle.
 */
#include <kern/processor.h>
#include <kern/recount.h>

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
