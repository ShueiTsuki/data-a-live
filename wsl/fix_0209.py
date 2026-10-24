W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# In Apple's arm64 assembler syntax (Mach-O) ';' starts a comment, not a new instruction: the kernel's
# inline "a; b" assembles to "a" alone. 0208's "isb; mrs %0, cntvct_el0" never read the counter (logo1:
# 13734 of 13808 lines carried one stale register value), and the older "dc cvac, %0; dsb sy" in the
# log ring lost its dsb. One instruction per asm statement.
S = 'bsd/dev/sweet_ufs.c'
edit(S, '''	__asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(cnt));''',
     '''	__asm__ volatile ("isb" ::: "memory");   /* 0209: one per asm, ';' is a comment here */
	__asm__ volatile ("mrs %0, cntvct_el0" : "=r"(cnt));''')
edit(S, '''    __asm__ volatile("dc cvac, %0; dsb sy" :: "r"(&sweet_log_total) : "memory");''',
     '''    __asm__ volatile("dc cvac, %0" :: "r"(&sweet_log_total) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");''')
print('patched')
