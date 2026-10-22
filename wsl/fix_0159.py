def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# Which side draws the broken home screen (blank wallpaper picture, icons that change while
# scrolling): backboardd's CPU renderer, or our copy to the panel?
# 1. Every change in the swap's layer set is logged (format, source and destination
#    rectangles, which layers our copy did not draw), at most 300 lines.
# 2. pandora_fbdump=SECS: every SECS seconds after the first image, the first shown layer's
#    surface (what backboardd composed) and the panel memory (what is on screen), both
#    downscaled 3x, go to the second half of the pinned log file (blocks after the 4 MiB
#    ring), overwriting the previous dump; tools/fbdump.py turns them into PNGs.
patch('iokit/Kernel/arm/SweetFramebuffer.cpp', [
    ("""extern "C" int sweet_console_muted;     /* osfmk/console/serial_console.c */""",
     """extern "C" int sweet_console_muted;     /* osfmk/console/serial_console.c */
#if defined(PANDORA)
extern "C" uint8_t *pandora_fbdump_begin(uint32_t *cap);       /* bsd/dev/sweet_ufs.c */
extern "C" void pandora_fbdump_commit(uint32_t len);

/* Dump header (block 0 of the dump area); frames follow at 4096, BGRA, tightly packed. */
struct pandora_fbdump_head {
	char magic[8];                  /* "PANDFBDM" */
	uint32_t version, seq, frames, scale;
	uint64_t uptime_ms;
	struct {
		char name[16];
		uint32_t width, height, offset, format;
	} f[2];
};

/* Every 3rd pixel of every 3rd row of a 32-bit image into out; returns bytes written. */
static uint32_t
pandora_fbdump_scale(uint8_t *out, uint32_t cap, uintptr_t base, uint32_t rowBytes,
    uint32_t width, uint32_t height, uint32_t *ow, uint32_t *oh)
{
	uint32_t w = width / 3, h = height / 3;
	if ((uint64_t)w * h * 4 > cap) {
		*ow = *oh = 0;
		return 0;
	}
	uint32_t *o = (uint32_t *)out;
	for (uint32_t y = 0; y < h; y++) {
		const uint32_t *row = (const uint32_t *)(base + (uint64_t)y * 3 * rowBytes);
		for (uint32_t x = 0; x < w; x++) {
			*o++ = row[x * 3];
		}
	}
	*ow = w;
	*oh = h;
	return w * h * 4;
}
#endif"""),
    ("""	bool gpuFrame = false, cpuFrame = false;
	bool shows = false;
""",
     """	bool gpuFrame = false, cpuFrame = false;
	bool shows = false;
#if defined(PANDORA)
	static char layersLast[320];
	static uint32_t layersLogged;
	static int dumpSecs = -1;
	static uint64_t dumpEvery, dumpNext, dumpSeq;
	char layers[320];
	size_t layersLen = 0;
	uint32_t drawnMask = 0;
	uint8_t *dump = NULL;
	uint32_t dumpCap = 0, dumpLen = 4096;
	struct pandora_fbdump_head *dh = NULL;
	layers[0] = 0;
	if (dumpSecs < 0) {
		dumpSecs = 0;
		PE_parse_boot_argn("pandora_fbdump", &dumpSecs, sizeof(dumpSecs));
		nanoseconds_to_absolutetime((uint64_t)dumpSecs * NSEC_PER_SEC, &dumpEvery);
	}
	if (dumpSecs > 0 && fConsolePaused && t0 >= dumpNext) {
		dump = pandora_fbdump_begin(&dumpCap);
		if (dump != NULL && dumpCap > 4096) {
			bzero(dump, 4096);
			dh = (struct pandora_fbdump_head *)dump;
			memcpy(dh->magic, "PANDFBDM", 8);
			dh->version = 1;
			dh->seq = (uint32_t)++dumpSeq;
			dh->scale = 3;
			uint64_t ns;
			absolutetime_to_nanoseconds(t0, &ns);
			dh->uptime_ms = ns / NSEC_PER_MSEC;
			dumpNext = t0 + dumpEvery;
		} else if (dump != NULL) {
			pandora_fbdump_commit(0);
			dump = NULL;
		}
	}
#endif
"""),
    ("""		SweetSurface *s = sweet_surface_copy(id);
		if (sweet_fb_swaps_logged < 16) {""",
     """		SweetSurface *s = sweet_surface_copy(id);
#if defined(PANDORA)
		if (layersLen < sizeof(layers)) {
			uint32_t f = s ? s->pixelFormat : 0;
			int n = snprintf(layers + layersLen, sizeof(layers) - layersLen,
			    " [%u %c%c%c%c %ux%u a %d,%d %ux%u b %d,%d %dx%d]", layer,
			    (char)(f >> 24), (char)(f >> 16), (char)(f >> 8), (char)f,
			    s ? s->width : 0, s ? s->height : 0, sx, sy, get32(a, 8), get32(a, 12),
			    dx, dy, w, h);
			if (n > 0) {
				layersLen = MIN(layersLen + (size_t)n, sizeof(layers));
			}
		}
		if (dh != NULL && dh->frames == 0 && s != NULL && s != fSurface && s->kernelAddress() &&
		    (s->bytesPerElement == 4 || s->bytesPerElement == 0) && s->memory != NULL &&
		    (uint64_t)s->offset + (uint64_t)s->height * s->bytesPerRow <= s->memory->getLength()) {
			uint32_t ow, oh, n = pandora_fbdump_scale(dump + dumpLen, dumpCap - dumpLen,
			    s->kernelAddress() + s->offset, s->bytesPerRow, s->width, s->height, &ow, &oh);
			if (n) {
				strlcpy(dh->f[0].name, "surface", sizeof(dh->f[0].name));
				dh->f[0].width = ow;
				dh->f[0].height = oh;
				dh->f[0].offset = dumpLen;
				dh->f[0].format = s->pixelFormat;
				dh->frames = 1;
				dumpLen += n;
			}
		}
#endif
		if (sweet_fb_swaps_logged < 16) {"""),
    ("""				cpuFrame = true;
			}
			s->release();
			continue;""",
     """				cpuFrame = true;
#if defined(PANDORA)
				drawnMask |= 1u << layer;
#endif
			}
			s->release();
			continue;"""),
    ("""		if (src != 0 && bpe == 4) {
			int copied = 0;""",
     """		if (src != 0 && bpe == 4) {
			int copied = 0;
#if defined(PANDORA)
			drawnMask |= 1u << layer;
#endif"""),
    ("""	}
#if defined(FUXI)
	if (!gpuFrame) fuxi_display_kick();""",
     """	}
#if defined(PANDORA)
	if (layersLen && strncmp(layers, layersLast, sizeof(layers)) != 0 && layersLogged < 300) {
		layersLogged++;
		strlcpy(layersLast, layers, sizeof(layersLast));
		IOLog("SWEET FB: layers mask 0x%x drawn 0x%x gpu %d:%s\\n", mask, drawnMask, gpuFrame,
		    layers);
	}
	if (dh != NULL) {
		uint32_t ow, oh, n = pandora_fbdump_scale(dump + dumpLen, dumpCap - dumpLen,
		    (uintptr_t)fPixels, fRowBytes, fWidth, fHeight, &ow, &oh);
		if (n) {
			uint32_t i = dh->frames;
			strlcpy(dh->f[i].name, "panel", sizeof(dh->f[i].name));
			dh->f[i].width = ow;
			dh->f[i].height = oh;
			dh->f[i].offset = dumpLen;
			dh->f[i].format = 0;
			dh->frames = i + 1;
			dumpLen += n;
		}
		pandora_fbdump_commit(dumpLen);
	}
#endif
#if defined(FUXI)
	if (!gpuFrame) fuxi_display_kick();"""),
])

patch('bsd/dev/sweet_ufs.c', [
    ("""static int
pandora_halt_restart(unsigned int type)
{""",
     """/*
 * Frame dumps (pandora_fbdump, iokit/Kernel/arm/SweetFramebuffer.cpp) go to the log file
 * after its header and 4 MiB ring. State: 0 free, 1 being filled by AppleCLCD::present,
 * 2 waiting for the log thread to write it.
 */
#define PFD_FIRST_BLOCK (1 + SWEET_LOG_SIZE / UFS_BLOCK)
#define PFD_SIZE        (SWEET_LOG_SIZE - UFS_BLOCK)
static uint8_t pfd_buf[PFD_SIZE];
static _Atomic uint32_t pfd_state;
static uint32_t pfd_len;

uint8_t *pandora_fbdump_begin(uint32_t *cap);
void pandora_fbdump_commit(uint32_t len);

uint8_t *
pandora_fbdump_begin(uint32_t *cap)
{
	uint32_t expected = 0;
	if (!pl_ready || pl_blocks < PFD_FIRST_BLOCK + PFD_SIZE / UFS_BLOCK ||
	    !os_atomic_cmpxchg(&pfd_state, expected, 1, acquire)) {
		return NULL;
	}
	*cap = PFD_SIZE;
	return pfd_buf;
}

void
pandora_fbdump_commit(uint32_t len)
{
	pfd_len = MIN(len, (uint32_t)PFD_SIZE);
	os_atomic_store(&pfd_state, len ? 2 : 0, release);
}

/* Write a finished dump. Caller owns the UFS host. */
static int
pfd_write(void)
{
	void *bounce = (void *)(su.dma + DMA_BOUNCE);
	uint32_t blocks = (pfd_len + UFS_BLOCK - 1) / UFS_BLOCK, done = 0;
	int error = 0;
	while (done < blocks && !error) {
		uint64_t lba, run;
		if (!pl_map(PFD_FIRST_BLOCK + done, &lba, &run)) {
			return EINVAL;
		}
		uint32_t n = (uint32_t)MIN(MIN((uint64_t)(blocks - done), run),
		    (uint64_t)(BOUNCE_SIZE / UFS_BLOCK));
		bzero(bounce, (size_t)n * UFS_BLOCK);
		memcpy(bounce, pfd_buf + (size_t)done * UFS_BLOCK,
		    MIN((size_t)n * UFS_BLOCK, (size_t)pfd_len - (size_t)done * UFS_BLOCK));
		error = ufs_write_blocks(lba, n);
		done += n;
	}
	return error ? error : ufs_sync_cache();
}

static int
pandora_halt_restart(unsigned int type)
{"""),
    ("""		int error = pl_flush();
		lck_mtx_unlock(&sweet_ufs_lock);
		if (error) {
			printf("PANDORA LOG: write failed (%d), log stopped\\n", error);""",
     """		int error = pl_flush();
		if (!error && os_atomic_load(&pfd_state, acquire) == 2) {
			int e = pfd_write();
			printf("PANDORA FBDUMP: %u bytes -> log file block %u: %d\\n", pfd_len,
			    PFD_FIRST_BLOCK, e);
			os_atomic_store(&pfd_state, 0, release);
		}
		lck_mtx_unlock(&sweet_ufs_lock);
		if (error) {
			printf("PANDORA LOG: write failed (%d), log stopped\\n", error);"""),
])
patch('bsd/dev/sweet_ufs.c', [
    ("""	uint64_t io_every = 0, io_last = start;
""",
     """	uint64_t io_every = 0, io_last = start;
	if (pl_blocks > PFD_FIRST_BLOCK) {
		/* No dump from an earlier boot may be taken for this one's. */
		uint64_t lba, run;
		lck_mtx_lock(&sweet_ufs_lock);
		if (pl_map(PFD_FIRST_BLOCK, &lba, &run)) {
			bzero((void *)(su.dma + DMA_BOUNCE), UFS_BLOCK);
			(void)ufs_write_blocks(lba, 1);
		}
		lck_mtx_unlock(&sweet_ufs_lock);
	}
"""),
])
print('patched')
