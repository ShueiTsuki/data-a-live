W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- 0217, second part: the display scans out iOS's frames (bsd/dev/pandora_dpu.c owns the hardware) -----------
# With Metal the frame CoreAnimation swaps is a GPU-written BGRA surface of the presented size. The pipes are given
# that surface (an IOVA in the display's SMMU bank) instead of the CPU copying it into the panel memory; the
# surface the pipes read and the one just given each hold a use count (fix_0217.py), so CoreAnimation draws the
# next frame elsewhere; a hold goes at the vsync tick that sees the pipes on the next frame. Console, display
# off, several layers or other surfaces: the pipes go back to the panel memory and the copy runs as before.
copy('pandora_dpu.c', 'bsd/dev/pandora_dpu.c')

# The log file written now, from a thread (pandora_dpu.c before it switches the display's SMMU route): r2b's
# kernel died early without a trace, and a hang leaves the last seconds of the ring unwritten.
U = 'bsd/dev/sweet_ufs.c'
edit(U, '''static void
pandora_log_start(void)
{''', '''/* 0217: write the ring to the log file now (thread context; false when there is no log file). */
bool pandora_log_sync(void);
bool
pandora_log_sync(void)
{
	if (!pl_ready) {
		return false;
	}
	lck_mtx_lock(&sweet_ufs_lock);
	int error = pl_flush();
	lck_mtx_unlock(&sweet_ufs_lock);
	return error == 0;
}

static void
pandora_log_start(void)
{''')
F = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
edit(F, '''extern "C" void pandora_dpu_probe(uint64_t fb);                 /* bsd/dev/pandora_dpu.c (0215) */''',
     '''extern "C" void pandora_dpu_probe(uint64_t fb);                 /* bsd/dev/pandora_dpu.c (0215) */
/* bsd/dev/pandora_dpu.c (0217): the pipes scan out a surface themselves. */
extern "C" int pandora_scanout_init(void);
extern "C" uint32_t pandora_scanout_map(const uint64_t *pages, uint32_t npages);
extern "C" void pandora_scanout_unmap(uint32_t iova, uint32_t npages);
extern "C" bool pandora_scanout_show(uint32_t addr, uint32_t stride, uint32_t w, uint32_t h, bool fill);
extern "C" bool pandora_scanout_panel(void);
extern "C" bool pandora_scanout_current(uint32_t addr);
extern "C" bool pandora_scanout_ready(void);
extern "C" void pandora_scanout_counts(uint64_t *shows, uint64_t *faults);
#define SWEET_SCAN_MAPS 6''')
edit(F, '''	uint32_t      fStatGpuWait;   /* PANDORA: flush ticks that waited for the GPU */
''', '''	uint32_t      fStatGpuWait;   /* PANDORA: flush ticks that waited for the GPU */
#if defined(PANDORA)
	/* 0217: scanout. Each of shown / queued holds one retain and one IOSurface use count. */
	bool          fScanWanted;    /* Metal on and pandora_scanout not 0: try at the first frame that fits */
	bool          fScanTried;
	bool          fScanOn;
	bool          fScanToPanel;   /* the pipes were sent back to the panel memory; holds go when that shows */
	SweetSurface *fScanShown;     /* the surface the pipes read */
	SweetSurface *fScanQueued;    /* the surface the pipes were just given (not yet seen as current) */
	uint32_t      fScanQueuedAddr;
	struct { SweetSurface *s; uint32_t iova, pages; uint64_t used; } fScanMaps[SWEET_SCAN_MAPS];
	uint64_t      fScanClock;
	uint32_t      fScanFrames, fScanCopies, fScanRefused;
#endif
''')
edit(F, '''	void swap(const uint8_t *rec);
	void present(const uint8_t *rec);
};''', '''	void swap(const uint8_t *rec);
	void present(const uint8_t *rec);
#if defined(PANDORA)
	uint32_t scanIova(SweetSurface *s);
	bool scanPresent(const uint8_t *rec, uint32_t mask);
	void scanPanel(void);
	void scanTick(void);
#endif
};''')
edit(F, '''#if defined(PANDORA)
	pandora_dpu_probe(fPhys);
#endif''', '''#if defined(PANDORA)
	pandora_dpu_probe(fPhys);
	/* 0217: with Metal the frames are GPU-written (in memory, not in CPU caches): the pipes can read them.
	 * The display's SMMU route is switched at the first frame that fits (scanPresent), when the log file
	 * is being written, not here. */
	fScanWanted = sweetfb_gpu_frames();
#endif''')
edit(F, '''	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {''', '''#if defined(PANDORA)
	fb->scanTick();
	if (fb->fPowerOff) {
		fb->scanPanel();   /* the fade below draws into the panel memory */
	}
#endif
	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {''')
edit(F, '''void
AppleCLCD::present(const uint8_t *rec)
{''', r'''#if defined(PANDORA)
static void
sweet_scan_hold(SweetSurface *s)
{
	s->retain();
	sweet_surface_use(s, 1);
}

static void
sweet_scan_drop(SweetSurface **s)
{
	if (*s != NULL) {
		sweet_surface_use(*s, -1);
		(*s)->release();
		*s = NULL;
	}
}

/* 0217: the IOVA of S's memory for the display (mapped on first use, kept while it keeps being swapped), or 0. */
uint32_t
AppleCLCD::scanIova(SweetSurface *s)
{
	unsigned slot = SWEET_SCAN_MAPS;
	fScanClock++;
	for (unsigned i = 0; i < SWEET_SCAN_MAPS; i++) {
		if (fScanMaps[i].s == s) {
			fScanMaps[i].used = fScanClock;
			return fScanMaps[i].iova;
		}
	}
	/* A slot: a free one, else the one longest unused that the pipes do not hold. */
	for (unsigned i = 0; i < SWEET_SCAN_MAPS; i++) {
		SweetSurface *t = fScanMaps[i].s;
		if (t == NULL) {
			slot = i;
			break;
		}
		if (t != fScanShown && t != fScanQueued && (slot == SWEET_SCAN_MAPS || fScanMaps[i].used < fScanMaps[slot].used)) {
			slot = i;
		}
	}
	if (slot == SWEET_SCAN_MAPS) {
		return 0;
	}
	if (fScanMaps[slot].s != NULL) {
		pandora_scanout_unmap(fScanMaps[slot].iova, fScanMaps[slot].pages);
		OSSafeReleaseNULL(fScanMaps[slot].s);
	}
	const uint64_t len = s->memory->getLength();
	const uint32_t n = (uint32_t)((len + 0xfff) >> 12);
	uint64_t *pages = (uint64_t *)IOMallocData((vm_size_t)n * sizeof(uint64_t));
	if (pages == NULL) {
		return 0;
	}
	bool ok = true;
	for (uint32_t i = 0; i < n && ok; i++) {
		IOByteCount seg = 0;
		const addr64_t pa = s->memory->getPhysicalSegment((IOByteCount)i << 12, &seg, kIOMemoryMapperNone);
		ok = pa != 0 && (pa & 0xfff) == 0;
		pages[i] = pa;
	}
	const uint32_t iova = ok ? pandora_scanout_map(pages, n) : 0;
	IOFreeData(pages, (vm_size_t)n * sizeof(uint64_t));
	if (iova == 0) {
		return 0;
	}
	s->retain();
	fScanMaps[slot].s = s;
	fScanMaps[slot].iova = iova;
	fScanMaps[slot].pages = n;
	fScanMaps[slot].used = fScanClock;
	IOLog("SWEET FB: scanout maps surface %u (%ux%u, %u pages) at iova %#x\n", s->id, s->width, s->height, n, iova);
	return iova;
}

/*
 * 0217: a frame the pipes take as it is: one layer, the whole presented display, a BGRA surface of exactly that
 * size, the console paused, display on, brightness on the panel (DCS, sweet_fb_dim untouched). True when the
 * pipes were given it.
 */
bool
AppleCLCD::scanPresent(const uint8_t *rec, uint32_t mask)
{
	if (!fScanWanted || !fConsolePaused || fPowerOff || sweet_fb_dim < 256) {
		return false;
	}
	if (!fScanOn) {
		if (fScanTried) {
			return false;
		}
		fScanTried = true;
		fScanOn = pandora_scanout_init() == 0;
		if (!fScanOn) {
			return false;
		}
	}
	uint32_t layer = SWEET_FB_LAYERS, layers = 0;
	for (uint32_t l = 0; l < SWEET_FB_LAYERS; l++) {
		if ((mask & (1u << l)) && get32(rec, 0x54 + 4 * l) != 0) {
			layer = l;
			layers++;
		}
	}
	if (layers != 1) {
		return false;
	}
	const uint8_t *a = rec + 0x64 + 16 * layer, *b = rec + 0xc4 + 16 * layer;
	SweetSurface *s = sweet_surface_copy(get32(rec, 0x54 + 4 * layer));
	const bool ok = s != NULL && s != fSurface && s->memory != NULL && s->pixelFormat == SWEET_FB_FORMAT_BGRA &&
	    s->bytesPerElement == 4 && s->elementWidth == 1 && s->elementHeight == 1 && s->width == fLogicalW &&
	    s->height == fLogicalH && s->bytesPerRow < 0x10000 && (s->offset & 0xf) == 0 &&
	    (uint64_t)s->offset + (uint64_t)s->bytesPerRow * s->height <= s->memory->getLength() &&
	    get32(a, 0) == 0 && get32(a, 4) == 0 && get32(a, 8) == s->width && get32(a, 12) == s->height &&
	    get32(b, 0) == 0 && get32(b, 4) == 0 && get32(b, 8) == fLogicalW && get32(b, 12) == fLogicalH;
	const uint32_t iova = ok ? scanIova(s) : 0;
	if (iova == 0 || !pandora_scanout_show(iova + s->offset, s->bytesPerRow, s->width, s->height, fFill)) {
		if (ok && fScanRefused++ < 8) {
			IOLog("SWEET FB: scanout refused surface %u (map %#x)\n", s->id, iova);
		}
		OSSafeReleaseNULL(s);
		return false;
	}
	/* show() waited for the earlier queued frame's flush: the pipes read that one now, the one before it no more. */
	if (fScanQueued != NULL) {
		sweet_scan_drop(&fScanShown);
		fScanShown = fScanQueued;
		fScanQueued = NULL;
	}
	sweet_scan_hold(s);
	fScanQueued = s;
	fScanQueuedAddr = iova + s->offset;
	fScanToPanel = false;
	s->release();
	return true;
}

/* 0217: the pipes back on the panel memory; the surface holds go once that is on screen (scanTick). */
void
AppleCLCD::scanPanel(void)
{
	if (fScanOn && !fScanToPanel && (fScanShown != NULL || fScanQueued != NULL) && pandora_scanout_panel()) {
		fScanToPanel = true;
	}
}

/* 0217: each vsync tick, drop what the pipes no longer read. */
void
AppleCLCD::scanTick(void)
{
	if (!fScanOn) {
		return;
	}
	if (fScanToPanel) {
		if (pandora_scanout_current(0)) {
			sweet_scan_drop(&fScanShown);
			sweet_scan_drop(&fScanQueued);
			fScanToPanel = false;
		}
		return;
	}
	if (fScanQueued != NULL && pandora_scanout_current(fScanQueuedAddr)) {
		sweet_scan_drop(&fScanShown);
		fScanShown = fScanQueued;
		fScanQueued = NULL;
	}
}
#endif

void
AppleCLCD::present(const uint8_t *rec)
{''')
edit(F, '''
	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);''', '''
#if defined(PANDORA)
	/* 0217: the pipes take the frame themselves when they can; else the panel memory and the copy below. */
	const bool scanned = shows && scanPresent(rec, mask);
	if (shows && !scanned) {
		scanPanel();
	}
	if (shows) {
		if (scanned) {
			fScanFrames++;
		} else {
			fScanCopies++;
		}
	}
#endif
	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);''')
edit(F, '''		if (s == NULL || s == fSurface) {
			OSSafeReleaseNULL(s);
			continue;
		}
		uintptr_t src = s->kernelAddress();''', '''#if defined(PANDORA)
		if (scanned) {   /* 0217: on the pipes already, nothing to copy */
			OSSafeReleaseNULL(s);
			drawnMask |= 1u << layer;
			continue;
		}
#endif
		if (s == NULL || s == fSurface) {
			OSSafeReleaseNULL(s);
			continue;
		}
		uintptr_t src = s->kernelAddress();''')
edit(F, '''		uint32_t ow, oh, n = pandora_fbdump_scale(dump + dumpLen, dumpCap - dumpLen,
		    (uintptr_t)fPixels, fRowBytes, fWidth, fHeight, &ow, &oh);''', '''		/* 0217: a scanned-out frame is not in the panel memory */
		uint32_t ow = 0, oh = 0, n = scanned ? 0 : pandora_fbdump_scale(dump + dumpLen, dumpCap - dumpLen,
		    (uintptr_t)fPixels, fRowBytes, fWidth, fHeight, &ow, &oh);''')
edit(F, '''		    avg_ns / 1000, max_ns / 1000, swaps, dropped, gpuWait);''', '''		    avg_ns / 1000, max_ns / 1000, swaps, dropped, gpuWait);
#if defined(PANDORA)
		uint64_t scanShows = 0, scanFaults = 0;
		pandora_scanout_counts(&scanShows, &scanFaults);
		IOLog("SWEET FB: scanout %s: %u frames scanned out, %u copied; %llu pipe updates, %llu SMMU faults, %u refused\\n",
		    fScanOn ? "on" : "off", fScanFrames, fScanCopies, scanShows, scanFaults, fScanRefused);
		fScanFrames = fScanCopies = 0;
#endif''')
print('patched (scanout)')
