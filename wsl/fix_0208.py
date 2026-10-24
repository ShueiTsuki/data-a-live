W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# Boot time (the "fast boot into iOS" work): freq5's user-space stamps put launchd at about 75 s after
# the kernel started and SpringBoard at 86 s, but kernel lines carry no time, so nothing tells where the
# time goes. Every line of the persistent log now starts with the time since its first byte,
# "[    12.345] " (seconds.milliseconds), from the generic timer (CNTVCT / CNTFRQ: valid from the
# first instruction, unlike absolutetime_to_nanoseconds before rtclock_init). User-space lines that
# reach the log get the same stamp, so kernel and user space share one timeline.
S = 'bsd/dev/sweet_ufs.c'
edit(S, '''
void
sweet_log_append(const char *c, int size)
{
	if (size <= 0) {
		return;
	}
	uint64_t at = os_atomic_add_orig(&sweet_log_total, (uint64_t)size, relaxed);''',
     '''
static void
sweet_log_put(const char *c, int size)
{
	if (size <= 0) {
		return;
	}
	uint64_t at = os_atomic_add_orig(&sweet_log_total, (uint64_t)size, relaxed);''')
edit(S, '''#if defined(FUXI)
    __asm__ volatile("dc cvac, %0; dsb sy" :: "r"(&sweet_log_total) : "memory");
#endif
}
''', '''#if defined(FUXI)
    __asm__ volatile("dc cvac, %0; dsb sy" :: "r"(&sweet_log_total) : "memory");
#endif
}

/* 0208: each line starts with "[ssssss.mmm] ", the time since the first log byte. */
static uint64_t sweet_log_t0;
static bool sweet_log_bol = true;

static void
sweet_log_stamp(void)
{
	uint64_t cnt, frq;
	__asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(cnt));
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(frq));
	if (sweet_log_t0 == 0) {
		sweet_log_t0 = cnt;
	}
	if (frq == 0) {
		return;
	}
	uint64_t ms = (cnt - sweet_log_t0) / (frq / 1000 ? frq / 1000 : 1);
	char p[14] = "[      .   ] ";
	uint64_t s = ms / 1000;
	for (int i = 3, v = (int)(ms % 1000); i >= 1; i--, v /= 10) {
		p[8 + i - 1] = (char)('0' + v % 10);
	}
	for (int i = 6; i >= 1; i--) {
		p[i] = (char)('0' + s % 10);
		s /= 10;
		if (s == 0) {
			break;
		}
	}
	sweet_log_put(p, 13);
}

void
sweet_log_append(const char *c, int size)
{
	int i = 0;
	while (i < size) {
		if (sweet_log_bol) {
			sweet_log_bol = false;
			sweet_log_stamp();
		}
		int j = i;
		while (j < size && c[j] != '\\n') {
			j++;
		}
		if (j < size) {
			j++;
			sweet_log_bol = true;
		}
		sweet_log_put(c + i, j - i);
		i = j;
	}
}
''')
print('patched')
