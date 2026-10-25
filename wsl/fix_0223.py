W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- 0223: call stacks in the sampling profiler --------------------------------------------------------------------
# The samples' PCs say where the time goes, not who asked for it: malloc, memmove and the kernel's GPU calls are each
# reached from many places. Kernel samples now keep LR and up to 5 frame records' returns; and each sample sets the
# thread's kperf AST, as kperf's samplers do, so the user call stack is walked in the thread's own context before it
# returns to user mode (kperf is not configured on this kernel, so the AST is the profiler's alone).
copy('pandora_prof.c', 'osfmk/arm/pandora_prof.c')
edit('osfmk/kern/ast.c', '''	if (reasons & AST_KPERF) {
		thread_ast_clear(thread, AST_KPERF);
		kperf_kpc_thread_ast(thread);
	}
''', '''	if (reasons & AST_KPERF) {
		thread_ast_clear(thread, AST_KPERF);
#if defined(PANDORA)
		extern void pandora_prof_ast(thread_t);
		pandora_prof_ast(thread);
#endif
		kperf_kpc_thread_ast(thread);
	}
''')
print('patched (profiler call stacks)')
