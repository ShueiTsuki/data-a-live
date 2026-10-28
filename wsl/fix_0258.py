W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0258: the console ring's readers see only finished bytes ----------------------------------------------------
# usb9: the USB serial log had 13 NULs where the disk log has a line's timestamp. sweet_log_put takes its place in the
# ring (sweet_log_total) before it stores the bytes, and the serial thread copied the place before the bytes were
# there. Writers now also count the bytes they have finished (sweet_log_done, release); sweet_log_copy goes only up
# to a moment when the two counts were equal. The last copy before a reset (pandora_usb_final) uses the new
# sweet_log_copy_any, up to every byte taken: a CPU a panic stopped inside sweet_log_put would otherwise hold back
# everything after it, the panic's own lines included.
copy('pandora_usb.c', 'bsd/dev/pandora_usb.c')

U = 'bsd/dev/sweet_ufs.c'
edit(U, '''static char sweet_log_ring[SWEET_LOG_SIZE];
static uint64_t _Atomic sweet_log_total;
''', '''static char sweet_log_ring[SWEET_LOG_SIZE];
static uint64_t _Atomic sweet_log_total;
static uint64_t _Atomic sweet_log_done;     /* 0258: bytes whose writer has stored them (sweet_log_copy) */
''')
edit(U, '''#if defined(FUXI)
    __asm__ volatile("dc cvac, %0" :: "r"(&sweet_log_total) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}''', '''#if defined(FUXI)
    __asm__ volatile("dc cvac, %0" :: "r"(&sweet_log_total) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#endif
	os_atomic_add(&sweet_log_done, (uint64_t)size, release);
}''')
edit(U, '''/*
 * Copy console bytes [*pos, total) (at most max) for live log streaming
 * (sweet_usb.c). A reader that fell more than the ring behind skips ahead.
 * Returns the byte count; *pos advances.
 */
uint32_t sweet_log_copy(uint64_t *pos, char *out, uint32_t max);

uint32_t
sweet_log_copy(uint64_t *pos, char *out, uint32_t max)
{
	uint64_t total = os_atomic_load(&sweet_log_total, relaxed);
	if (total - *pos > SWEET_LOG_SIZE) {''', '''/*
 * Copy console bytes [*pos, end) (at most max) for live log streaming
 * (sweet_usb.c, pandora_usb.c). A reader that fell more than the ring behind skips ahead.
 * Returns the byte count; *pos advances.
 * 0258: end is the last moment seen when every byte taken had been stored (sweet_log_done
 * == sweet_log_total): sweet_log_put takes its place before it stores the bytes, and usb9's
 * serial log got 13 NULs where a timestamp was still to come. sweet_log_copy_any goes up to
 * every byte taken, for the last copy before a reset.
 */
static uint64_t sweet_log_safe;

static uint32_t
sweet_log_copy_to(uint64_t *pos, char *out, uint32_t max, uint64_t total)
{
	if (total < *pos) {
		return 0;
	}
	if (total - *pos > SWEET_LOG_SIZE) {''')
edit(U, '''	*pos += n;
	return n;
}

uint64_t sweet_log_position(void);''', '''	*pos += n;
	return n;
}

uint32_t sweet_log_copy(uint64_t *pos, char *out, uint32_t max);

uint32_t
sweet_log_copy(uint64_t *pos, char *out, uint32_t max)
{
	const uint64_t done = os_atomic_load(&sweet_log_done, acquire);
	const uint64_t total = os_atomic_load(&sweet_log_total, relaxed);
	uint64_t safe = __atomic_load_n(&sweet_log_safe, __ATOMIC_RELAXED);
	while (done == total && safe < total &&
	    !__atomic_compare_exchange_n(&sweet_log_safe, &safe, total, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
	return sweet_log_copy_to(pos, out, max, __atomic_load_n(&sweet_log_safe, __ATOMIC_RELAXED));
}

uint32_t sweet_log_copy_any(uint64_t *pos, char *out, uint32_t max);

uint32_t
sweet_log_copy_any(uint64_t *pos, char *out, uint32_t max)
{
	return sweet_log_copy_to(pos, out, max, os_atomic_load(&sweet_log_total, relaxed));
}

uint64_t sweet_log_position(void);''')
print('patched (ring readers see only finished bytes)')
