W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60], s.count(o))
        s = s.replace(o, n)
    open(p, 'w').write(s)


# ---- 0308: screen shots on volume up + volume down ------------------------------------------------------------------
# The user sees words in Settings turn into other words and asked for a way to take pictures of the screen inside iOS.
# Volume up and volume down held together save what is on screen, whole, as /private/var/tmp/pandora_shot_N.bgra
# (pandora_files.c writes them in a thread of its own); tools/shot_fetch.py brings them back as PNGs.
copy('pandora_files.c', 'bsd/dev/pandora_files.c')

patch('bsd/dev/fuxi_keys.c', [
    ("""extern void sweet_hid_key(int key, int down);
""",
     """extern void sweet_hid_key(int key, int down);
#if defined(PANDORA)
extern void pandora_shot_request(void);         /* bsd/dev/pandora_files.c (0308) */
#endif
"""),
    ("""	int state[3] = {0, 0, 0}, last[3] = {0, 0, 0};
	unsigned errors = 0;
""",
     """	int state[3] = {0, 0, 0}, last[3] = {0, 0, 0};
	unsigned errors = 0;
#if defined(PANDORA)
	bool shot_chord = false;
#endif
"""),
    ("""			if (ok[k]) {
				last[k] = now[k];
			}
		}
		fk_sleep(20);""",
     """			if (ok[k]) {
				last[k] = now[k];
			}
		}
#if defined(PANDORA)
		/* 0308: volume up and volume down held together: one screen shot. The keys still go to iOS. */
		const bool chord = state[1] && state[2];
		if (chord && !shot_chord) {
			printf("PANDORA KEYS: volume up + volume down: screen shot\\n");
			pandora_shot_request();
		}
		shot_chord = chord;
#endif
		fk_sleep(20);"""),
])

patch('iokit/Kernel/arm/SweetFramebuffer.cpp', [
    ("""extern "C" void pandora_fbdump_commit(uint32_t len);
""",
     """extern "C" void pandora_fbdump_commit(uint32_t len);

/*
 * 0308: screen shots (bsd/dev/pandora_files.c, on volume up + volume down). The frame the pipes were last given
 * (scanPresent; held here until the next one), or the panel memory while the pipes read that, copied whole.
 */
static IOLock *pandora_shot_lock;
static SweetSurface *pandora_shot_surface;      /* retained; NULL: the panel memory is on screen */
static uintptr_t pandora_shot_panel;
static uint32_t pandora_shot_panel_w, pandora_shot_panel_h, pandora_shot_panel_rb;

static void
pandora_shot_note(SweetSurface *s)
{
	if (pandora_shot_lock == NULL) {
		return;
	}
	if (s != NULL) {
		s->retain();
	}
	IOLockLock(pandora_shot_lock);
	SweetSurface *old = pandora_shot_surface;
	pandora_shot_surface = s;
	IOLockUnlock(pandora_shot_lock);
	if (old != NULL) {
		old->release();
	}
}

extern "C" void *pandora_shot_take(uint32_t *len, uint32_t *w, uint32_t *h);
extern "C" void pandora_shot_free(void *p, uint32_t len);

/* What is on screen as tightly packed BGRA (free it with pandora_shot_free), or NULL. */
void *
pandora_shot_take(uint32_t *len, uint32_t *w, uint32_t *h)
{
	if (pandora_shot_lock == NULL) {
		return NULL;
	}
	IOLockLock(pandora_shot_lock);
	SweetSurface *s = pandora_shot_surface;
	if (s != NULL) {
		s->retain();
	}
	IOLockUnlock(pandora_shot_lock);
	uintptr_t base = pandora_shot_panel;
	uint32_t width = pandora_shot_panel_w, height = pandora_shot_panel_h, rowBytes = pandora_shot_panel_rb;
	if (s != NULL) {
		base = s->kernelAddress();
		base = base != 0 ? base + s->offset : 0;
		width = s->width;
		height = s->height;
		rowBytes = s->bytesPerRow;
	}
	const uint64_t n = (uint64_t)width * height * 4;
	uint8_t *out = NULL;
	if (base != 0 && width != 0 && height != 0 && rowBytes >= width * 4 && n <= (64u << 20)) {
		out = (uint8_t *)IOMallocData((vm_size_t)n);
	}
	for (uint32_t y = 0; out != NULL && y < height; y++) {
		memcpy(out + (uint64_t)y * width * 4, (const void *)(base + (uint64_t)y * rowBytes), (size_t)width * 4);
	}
	OSSafeReleaseNULL(s);
	if (out != NULL) {
		*len = (uint32_t)n;
		*w = width;
		*h = height;
	}
	return out;
}

void
pandora_shot_free(void *p, uint32_t len)
{
	IOFreeData(p, len);
}
"""),
    ("""	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	fWorkLoop = IOWorkLoop::workLoop();
""",
     """	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
#if defined(PANDORA)
	pandora_shot_lock = IOLockAlloc();
	pandora_shot_panel = fPixels;
	pandora_shot_panel_w = fWidth;
	pandora_shot_panel_h = fHeight;
	pandora_shot_panel_rb = fRowBytes;
#endif
	fWorkLoop = IOWorkLoop::workLoop();
"""),
    ("""	fScanQueued = s;
	fScanQueuedAddr = iova + s->offset;
""",
     """	fScanQueued = s;
	fScanQueuedAddr = iova + s->offset;
#if defined(PANDORA)
	pandora_shot_note(s);
#endif
"""),
    ("""		if (pandora_scanout_current(0)) {
			sweet_scan_drop(&fScanShown);
			sweet_scan_drop(&fScanQueued);
			fScanToPanel = false;
""",
     """		if (pandora_scanout_current(0)) {
			sweet_scan_drop(&fScanShown);
			sweet_scan_drop(&fScanQueued);
			fScanToPanel = false;
#if defined(PANDORA)
			pandora_shot_note(NULL);
#endif
"""),
])
print('patched (0308 screen shots)')
