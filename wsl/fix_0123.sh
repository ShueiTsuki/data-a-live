#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/kern/kern_exit.c'
t = open(p).read()
old = """	proc_crash_coredump(p);

	sync(p, (void *)NULL, (int *)NULL);
"""
new = """#if defined(PANDORA)
	/*
	 * pandora_initwait=N: when launchd dies, keep the console (dyld / shared region /
	 * ignition messages) on the screen for N seconds before the panic text covers it.
	 */
	uint32_t pandora_wait = 0;
	if (p == initproc && PE_parse_boot_argn("pandora_initwait", &pandora_wait, sizeof(pandora_wait)) &&
	    pandora_wait != 0) {
		if (pandora_wait > 600) {
			pandora_wait = 600;
		}
		printf("PANDORA: %s; waiting %u s before the panic\\n", prefix_str, pandora_wait);
		assert_wait_timeout((event_t)&pandora_wait, THREAD_UNINT, pandora_wait, NSEC_PER_SEC);
		thread_block(THREAD_CONTINUE_NULL);
	}
#endif

	proc_crash_coredump(p);

	sync(p, (void *)NULL, (int *)NULL);
"""
assert t.count(old) == 1
t = t.replace(old, new)
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0123-pandora-initproc-panic-wait.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0123
