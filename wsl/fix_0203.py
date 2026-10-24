W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# Big cores first (boot-arg pandora_bigfirst=1). This kernel is built without __ARM_AMP__ (one cluster, every CPU
# CLUSTER_TYPE_SMP), and choose_processor takes the lowest-numbered idle CPU (lsb_first), so work packs onto
# cpus 0-2, the slowest cores, while the two big cores (6, 7: 4.4 GHz, capacity 1024 against 762) sit idle (freq3:
# 0-1 % busy unless the whole machine is loaded). With the boot-arg the highest-numbered idle CPU is taken
# instead: 7 and 6 first, then 5 .. 0. Realtime threads and the rest of the choice are unchanged.
S = 'osfmk/kern/sched_prim.c'
edit(S, '''#if DEVELOPMENT || DEBUG
int enable_task_set_cluster_type = 0;
bool system_ecore_only = false;
#endif /* DEVELOPMENT || DEBUG */
''', '''#if DEVELOPMENT || DEBUG
int enable_task_set_cluster_type = 0;
bool system_ecore_only = false;
#endif /* DEVELOPMENT || DEBUG */

#if defined(PANDORA)
/* pandora: boot-arg pandora_bigfirst=1, an idle CPU is taken from the highest number down (the big cores 6-7). */
static int sched_pandora_bigfirst = 0;
#endif
''')
edit(S, '''	if (PE_parse_boot_argn("direct_handoff", &direct_handoff, sizeof(direct_handoff))) {
		allow_direct_handoff = direct_handoff;
	}
''', '''	if (PE_parse_boot_argn("direct_handoff", &direct_handoff, sizeof(direct_handoff))) {
		allow_direct_handoff = direct_handoff;
	}
#if defined(PANDORA)
	if (PE_parse_boot_argn("pandora_bigfirst", &sched_pandora_bigfirst, sizeof(sched_pandora_bigfirst))) {
		kprintf("Scheduler: pandora_bigfirst %d\\n", sched_pandora_bigfirst);
	}
#endif
''')
edit(S, '''		int cpuid = lsb_first(idle_primary_map);
		if (cpuid >= 0) {
			processor = processor_array[cpuid];
			return processor;
		}''', '''#if defined(PANDORA)
		int cpuid = sched_pandora_bigfirst ? bit_first(idle_primary_map) : lsb_first(idle_primary_map);
#else
		int cpuid = lsb_first(idle_primary_map);
#endif
		if (cpuid >= 0) {
			processor = processor_array[cpuid];
			return processor;
		}''')
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
print('patched')
