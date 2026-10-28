W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0263: brightness -------------------------------------------------------------------------------------------
# User: the brightness cannot be changed any more; the level set in iOS carries into the next boot's logo; and
# changes come in visible steps, not smoothly.
# 1. Since 0259 (usb10, usb11, spk7) CoreBrightness sets "brightness" 1 (panel level 7) at about 24 s, the first
#    round of thermalmonitord's backlight control (BackLightCC, iPhone X plist), and the slider stays at the bottom.
#    The iOS temperature sensors are now off unless pandora_therm_ios=1 (pandora_therm.c), until thermal step 2
#    gives thermalmonitord this phone's plist.
# 2. The panel's level before our first change is read back (DCS 0x52; 0x394 in every log so far) and put back
#    right before every restart (sweet_bl_final from pl_psci_reset), with the panel's dimming off again.
# 3. The panel's own dimming is turned on (DCS 0x53 = 0x28, the DT's mi,mdss-dsi-dimmingon-command; Android sends
#    it too): each level change fades in the panel. FadePeriod (seconds as 16.16, sent with "brightness"; 12124 =
#    0.185 s when the screen dims) is followed by a thread that steps "brightness" every 16 ms, linear over the
#    period. FadePeriod 0 changes at once (the panel's dimming smooths it).
copy('pandora_therm.c', 'bsd/dev/pandora_therm.c')

F = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
edit(F, '''static vm_offset_t sweet_dsi;
static int sweet_dsi_bl = -1;           /* -1 unknown, 0 off/failed, 1 working */
static unsigned sweet_dsi_logs;
''', '''static vm_offset_t sweet_dsi;
static int sweet_dsi_bl = -1;           /* -1 unknown, 0 off/failed, 1 working */
static unsigned sweet_dsi_logs;
/* 0263: one sender at a time in the command FIFO (the fade thread, the first setProperties, sweet_bl_final). */
static volatile uint32_t sweet_dsi_busy;
static uint32_t sweet_bl_boot;          /* the panel's level before our first change, 0 if not read */
static volatile int sweet_bl_stop;      /* set by sweet_bl_final: no more changes */
''')

edit(F, '''/* One command (or several packed) through the test-pattern FIFO; true when DMA done. */
static bool
sweet_dsi_send(const uint32_t *w, unsigned n)
{
''', '''static void
sweet_dsi_take(void)
{
	uint32_t idle = 0;
	while (!__atomic_compare_exchange_n(&sweet_dsi_busy, &idle, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		idle = 0;
		IODelay(1);
	}
}

static void
sweet_dsi_give(void)
{
	__atomic_store_n(&sweet_dsi_busy, 0u, __ATOMIC_RELEASE);
}

/* One command (or several packed) through the test-pattern FIFO; true when DMA done. Caller holds the FIFO. */
static bool
sweet_dsi_send_one(const uint32_t *w, unsigned n)
{
''')

edit(F, '''	if (done) {
		sweet_dsi_wr(0x110, (sweet_dsi_rd(0x110) & 0xeeffffffu) | 1u);
	}
	return done;
}

static bool
sweet_dsi_brightness(uint32_t level)
{
	if (sweet_dsi_bl < 0) {''', '''	if (done) {
		sweet_dsi_wr(0x110, (sweet_dsi_rd(0x110) & 0xeeffffffu) | 1u);
	}
	return done;
}

static bool
sweet_dsi_send(const uint32_t *w, unsigned n)
{
	sweet_dsi_take();
	bool done = sweet_dsi_send_one(w, n);
	sweet_dsi_give();
	return done;
}

/* DCS read 0x52: set maximum return packet size 2 (0x37), then the read (short, last + bus turn-around). The
 * answer, a DCS short read response 22 hi lo ecc, lands in RDBK_DATA0. 0 when no level came back. */
static uint32_t
sweet_dsi_read_level(void)
{
	uint32_t maxret = 0x80370002u, rd = 0xa0060052u;
	sweet_dsi_take();
	bool ok = sweet_dsi_send_one(&maxret, 1) && sweet_dsi_send_one(&rd, 1);
	IODelay(200);
	uint32_t d0 = sweet_dsi_rd(0x6c);
	sweet_dsi_give();
	uint32_t level = (d0 >> 8) & 0xffffu;
	return ok && (d0 >> 24) == 0x22u && level >= SWEET_PANEL_BL_MIN && level <= 0x3fffu ? level : 0;
}

/* DSI0 mapped and in command mode as ABL left it; the first call maps it, reads the panel's level and turns the
 * panel's dimming on. */
static bool
sweet_dsi_ready(void)
{
	if (sweet_dsi_bl < 0) {''')

edit(F, '''				if ((ctrl & 5u) != 5u) {
					IOLog("SWEET DSI: DSI0 not enabled in command mode; brightness by pixels\\n");
					sweet_dsi_bl = 0;
				}
			}
		}
	}
	if (sweet_dsi_bl != 1) {
		return false;
	}
	level = level < SWEET_PANEL_BL_MIN ? SWEET_PANEL_BL_MIN :
	    (level > SWEET_PANEL_BL_MAX ? SWEET_PANEL_BL_MAX : level);
	/* header: word count 3, 0, DCS long write 0x39, last + long; payload 51 hi lo, pad ff */
	uint32_t cmd[2] = {0xc0390003u,
		           0xff000000u | ((level & 0xffu) << 16) | (((level >> 8) & 0xffu) << 8) | 0x51u};
	bool done = sweet_dsi_send(cmd, 2);
	bool log = sweet_dsi_logs++ < 20 || !done;
	if (log) {
		IOLog("SWEET DSI: brightness %u (%#x) %s\\n", level, level, done ? "sent" : "TIMEOUT");
	}
	if (!done) {
		sweet_dsi_bl = 0;       /* fall back to pixel scaling for the rest of the boot */
		return false;
	}
	if (log) {
		/* Read it back: set maximum return packet size 2 (0x37), then DCS read 0x52
		 * (short, last + bus turn-around); the answer lands in RDBK_DATA0..3. */
		/* RDBK_DATA0 then holds 22 hi lo ecc (DCS short read response, 2 bytes). Read
		 * right away and again 100 ms later (frames from the MDP in between): the first
		 * test read the old level 0x394 200 us after the write. */
		uint32_t maxret = 0x80370002u, rd = 0xa0060052u;
		for (int pass = 0; pass < 2; pass++) {
			if (pass) {
				IOSleep(100);
			}
			bool r1 = sweet_dsi_send(&maxret, 1);
			bool r2 = r1 && sweet_dsi_send(&rd, 1);
			IODelay(200);
			uint32_t d0 = sweet_dsi_rd(0x6c);
			IOLog("SWEET DSI: read 0x52 (%s) %d/%d: count %#x ack_err %#x data %08x %08x -> level %u\\n",
			    pass ? "+100 ms" : "now", r1, r2, sweet_dsi_rd(0x1d4), sweet_dsi_rd(0x68), d0,
			    sweet_dsi_rd(0x70), (d0 >> 8) & 0xffffu);
		}
	}
	return true;
}
''', '''				if ((ctrl & 5u) != 5u) {
					IOLog("SWEET DSI: DSI0 not enabled in command mode; brightness by pixels\\n");
					sweet_dsi_bl = 0;
				}
			}
		}
		if (sweet_dsi_bl == 1) {
			sweet_bl_boot = sweet_dsi_read_level();
			/* the panel's dimming on: DCS short write 0x15, 53 28 (DT mi,mdss-dsi-dimmingon-command) */
			uint32_t dim = 0x80152853u;
			bool ok = sweet_dsi_send(&dim, 1);
			IOLog("SWEET DSI: panel level at start %u (%#x), panel dimming on %s\\n", sweet_bl_boot, sweet_bl_boot,
			    ok ? "sent" : "TIMEOUT");
		}
	}
	return sweet_dsi_bl == 1;
}

static bool
sweet_dsi_level(uint32_t level, bool log)
{
	level = level < SWEET_PANEL_BL_MIN ? SWEET_PANEL_BL_MIN :
	    (level > SWEET_PANEL_BL_MAX ? SWEET_PANEL_BL_MAX : level);
	/* header: word count 3, 0, DCS long write 0x39, last + long; payload 51 hi lo, pad ff */
	uint32_t cmd[2] = {0xc0390003u,
		           0xff000000u | ((level & 0xffu) << 16) | (((level >> 8) & 0xffu) << 8) | 0x51u};
	bool done = sweet_dsi_send(cmd, 2);
	if (log || !done) {
		IOLog("SWEET DSI: brightness %u (%#x) %s\\n", level, level, done ? "sent" : "TIMEOUT");
	}
	if (!done) {
		sweet_dsi_bl = 0;       /* fall back to pixel scaling for the rest of the boot */
	}
	return done;
}

/* "brightness" 0..65536 to the panel's level: square law over the normal range (50 % -> about a quarter). */
static uint32_t
sweet_bl_level(uint32_t frac)
{
	return SWEET_PANEL_BL_MIN + (uint32_t)((uint64_t)(SWEET_PANEL_BL_MAX - SWEET_PANEL_BL_MIN) * frac * frac /
	           (65536ULL * 65536ULL));
}

/* The fade: from where "brightness" is to the newest request, linear over its FadePeriod. */
static IOLock *sweet_bl_lock;
static int sweet_bl_threaded = -1;
static bool sweet_bl_new;
static uint32_t sweet_bl_cur = ~0u, sweet_bl_from, sweet_bl_to;
static uint64_t sweet_bl_t0, sweet_bl_len;      /* mach_absolute_time units */
static unsigned sweet_bl_fades;

static void
sweet_bl_thread(void *, wait_result_t)
{
	unsigned steps = 0;
	IOLockLock(sweet_bl_lock);
	for (;;) {
		while (!sweet_bl_stop && !sweet_bl_new && sweet_bl_cur == sweet_bl_to) {
			IOLockSleep(sweet_bl_lock, &sweet_bl_new, THREAD_UNINT);
		}
		if (sweet_bl_stop) {
			break;
		}
		sweet_bl_new = false;
		const uint64_t dt = mach_absolute_time() - sweet_bl_t0;
		uint32_t v = sweet_bl_to;
		if (dt < sweet_bl_len) {
			const int64_t d = (int64_t)sweet_bl_to - (int64_t)sweet_bl_from;
			v = (uint32_t)((int64_t)sweet_bl_from + d * (int64_t)dt / (int64_t)sweet_bl_len);
		}
		const uint32_t prev = sweet_bl_cur;
		const uint64_t len = sweet_bl_len;
		sweet_bl_cur = v;
		const bool last = v == sweet_bl_to;
		IOLockUnlock(sweet_bl_lock);
		if (prev == ~0u || sweet_bl_level(v) != sweet_bl_level(prev)) {
			(void)sweet_dsi_level(sweet_bl_level(v), false);
			steps++;
		}
		if (last) {
			if (sweet_bl_fades < 12) {
				sweet_bl_fades++;
				uint64_t ns;
				absolutetime_to_nanoseconds(len, &ns);
				IOLog("SWEET BL: at %u (level %u) after %u steps, fade %llu ms\\n", v, sweet_bl_level(v), steps,
				    ns / NSEC_PER_MSEC);
				if (sweet_bl_fades <= 2) {
					IOSleep(100);           /* frames from the MDP in between, then the panel answers */
					IOLog("SWEET DSI: panel reads level %u\\n", sweet_dsi_read_level());
				}
			}
			steps = 0;
		} else {
			IOSleep(16);
		}
		IOLockLock(sweet_bl_lock);
	}
	IOLockUnlock(sweet_bl_lock);
}

/* A new "brightness" with its FadePeriod (seconds, 16.16); false when the panel cannot be reached over DSI. */
static bool
sweet_bl_request(uint32_t frac, uint64_t fade)
{
	if (!sweet_dsi_ready() || sweet_bl_stop) {
		return false;
	}
	if (sweet_bl_threaded < 0) {
		thread_t t = THREAD_NULL;
		sweet_bl_lock = IOLockAlloc();
		sweet_bl_threaded = sweet_bl_lock != NULL &&
		    kernel_thread_start(sweet_bl_thread, NULL, &t) == KERN_SUCCESS;
		if (t != THREAD_NULL) {
			thread_deallocate(t);
		}
	}
	if (!sweet_bl_threaded) {
		return sweet_dsi_level(sweet_bl_level(frac), sweet_dsi_logs++ < 20);
	}
	uint64_t len = 0;
	if (fade) {
		fade = fade > (10ULL << 16) ? (10ULL << 16) : fade;     /* at most 10 s */
		nanoseconds_to_absolutetime((fade * NSEC_PER_SEC) >> 16, &len);
	}
	IOLockLock(sweet_bl_lock);
	sweet_bl_from = sweet_bl_cur == ~0u ? frac : sweet_bl_cur;
	sweet_bl_to = frac;
	sweet_bl_t0 = mach_absolute_time();
	sweet_bl_len = len;
	sweet_bl_new = true;
	IOLockWakeup(sweet_bl_lock, &sweet_bl_new, false);
	IOLockUnlock(sweet_bl_lock);
	return sweet_dsi_bl == 1;
}

/* Right before a restart (sweet_ufs.c pl_psci_reset, a panic's too): the panel's dimming off and its level from
 * before our first change, so the next boot's logo is not at the level iOS had. No locks: another CPU may be
 * stopped; one still sending gets 20 ms. */
extern "C" void
sweet_bl_final(void)
{
	if (sweet_dsi_bl != 1 || !sweet_bl_boot) {
		return;
	}
	sweet_bl_stop = 1;
	for (int i = 0; i < 20000 && __atomic_load_n(&sweet_dsi_busy, __ATOMIC_ACQUIRE); i++) {
		IODelay(1);
	}
	sweet_dsi_give();
	uint32_t off = 0x80152053u;     /* 53 20, mi,mdss-dsi-dimmingoff-command */
	bool ok = sweet_dsi_send(&off, 1);
	ok = sweet_dsi_level(sweet_bl_boot, false) && ok;
	printf("SWEET BL: restart: panel level %u, dimming off %s\\n", sweet_bl_boot, ok ? "sent" : "TIMEOUT");
}

static uint64_t sweet_bl_fade;          /* FadePeriod of the setProperties being applied */
''')

edit(F, '''	if (frac != ~0ULL) {
		frac = frac > 65536 ? 65536 : frac;
		/* Panel level: square law over the normal range (50 % -> about a quarter). */
		uint32_t level = SWEET_PANEL_BL_MIN + (uint32_t)((uint64_t)(SWEET_PANEL_BL_MAX -
		    SWEET_PANEL_BL_MIN) * frac * frac / (65536ULL * 65536ULL));
		if (sweet_dsi_brightness(level)) {
			sweet_fb_dim = 256;
			return;
		}
''', '''	if (frac != ~0ULL) {
		frac = frac > 65536 ? 65536 : frac;
		if (sweet_bl_request((uint32_t)frac, sweet_bl_fade)) {
			sweet_fb_dim = 256;
			return;
		}
''')

edit(F, '''	OSCollectionIterator *it = OSCollectionIterator::withCollection(d);
	const OSSymbol *k;
	while (it && (k = OSDynamicCast(OSSymbol, it->getNextObject()))) {
		sweet_bl_apply(k, d->getObject(k), 0);
	}
	OSSafeReleaseNULL(it);
	return kIOReturnSuccess;
}''', '''	/* FadePeriod comes in the same dictionary as brightness; take it first, whatever the order. */
	sweet_bl_fade = 0;
	(void)sweet_bl_number(d->getObject("FadePeriod"), &sweet_bl_fade);
	OSCollectionIterator *it = OSCollectionIterator::withCollection(d);
	const OSSymbol *k;
	while (it && (k = OSDynamicCast(OSSymbol, it->getNextObject()))) {
		sweet_bl_apply(k, d->getObject(k), 0);
	}
	OSSafeReleaseNULL(it);
	return kIOReturnSuccess;
}''')

U = 'bsd/dev/sweet_ufs.c'
edit(U, '''#if defined(PANDORA)
extern void pandora_usb_final(void);
#endif
''', '''#if defined(PANDORA)
extern void pandora_usb_final(void);
extern void sweet_bl_final(void);       /* SweetFramebuffer.cpp, 0263 */
#endif
''')
edit(U, '''#if defined(PANDORA)
	/* the USB serial log's last bytes (bsd/dev/pandora_usb.c; nothing without pandora_usb=2) */
	pandora_usb_final();
#endif''', '''#if defined(PANDORA)
	/* the panel back at its level from before iOS (0263), then the USB serial log's last bytes
	 * (bsd/dev/pandora_usb.c; nothing without pandora_usb=2) */
	sweet_bl_final();
	pandora_usb_final();
#endif''')
print('patched (0263 brightness)')
