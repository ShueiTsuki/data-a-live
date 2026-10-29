def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60], s.count(o))
        s = s.replace(o, n)
    open(p, 'w').write(s)


# ---- 0311: what each process does with IOSurfaces --------------------------------------------------------------------
# A row Settings just changed (tapped, its check and highlight right) shows for a moment the words of the row tapped
# before it (user: 「变成的文字是按的上一次的字」), with the render server on the GPU or on the CPU (shot5). Read as: the
# label is drawn into a buffer the previous row's label had, and the render server still shows what the buffer held
# before; it takes new contents from a surface whose seed (bumped by an unlock that is not read-only) changed. Counted
# per process: creates, locks, unlocks that bump the seed, read-only unlocks, use count +/-; a summary every 300 calls.
# Settings (Preferences): its first 40 creates and first 60 locks/unlocks one by one.
S = 'iokit/Kernel/arm/SweetSurface.cpp'
patch(S, [
    ("""static uint32_t      sweet_surface_creates_logged;
""",
     """static uint32_t      sweet_surface_creates_logged;

/* 0311: per-process counts of what is done with surfaces; a summary every 300 calls. */
enum { SN_CREATE, SN_LOCK, SN_UNLOCK, SN_UNLOCK_RO, SN_USE_INC, SN_USE_DEC, SN_USE_MISSED, SN_KINDS };
static IOLock *sweet_note_lock;

/* Counts KIND for the calling process; true when the caller is Settings (Preferences). */
static bool
sweet_note(unsigned kind)
{
	static struct {
		char name[20];
		uint32_t n[SN_KINDS];
	} who[10];
	static uint32_t calls;
	char name[20];
	proc_selfname(name, sizeof(name));
	if (sweet_note_lock == NULL) {
		return false;
	}
	IOLockLock(sweet_note_lock);
	unsigned i = 0;
	while (i < 10 && who[i].name[0] != 0 && strncmp(who[i].name, name, sizeof(name)) != 0) {
		i++;
	}
	if (i == 10) {
		i = 9;                          /* the last row takes everyone else */
	} else if (who[i].name[0] == 0) {
		strlcpy(who[i].name, name, sizeof(who[i].name));
	}
	who[i].n[kind]++;
	if (++calls % 300 == 0) {
		for (unsigned k = 0; k < 10 && who[k].name[0] != 0; k++) {
			IOLog("SWEET SURF: after %u: %s create %u lock %u unlock %u (read-only %u) use +%u -%u missed %u\\n", calls,
			    who[k].name, who[k].n[SN_CREATE], who[k].n[SN_LOCK], who[k].n[SN_UNLOCK], who[k].n[SN_UNLOCK_RO],
			    who[k].n[SN_USE_INC], who[k].n[SN_USE_DEC], who[k].n[SN_USE_MISSED]);
		}
	}
	IOLockUnlock(sweet_note_lock);
	return strcmp(name, "Preferences") == 0;
}
"""),
    ("""	if (ret != kIOReturnSuccess || sweet_surface_creates_logged < 64) {
		char name[32];
		proc_selfname(name, sizeof(name));
		sweet_surface_creates_logged++;
		IOLog("SWEET SURF: %s create id %u %ux%u fmt 0x%x bpe %u bpr %u planes %u size %u keys %u -> 0x%x\\n",
		    name, s->id, s->width, s->height, s->pixelFormat, s->bytesPerElement,
		    s->bytesPerRow, s->planeCount, s->allocSize, props->getCount(), ret);
	}""",
     """	static uint32_t prefsCreates;
	const bool prefs = sweet_note(SN_CREATE) && prefsCreates < 40;
	if (ret != kIOReturnSuccess || sweet_surface_creates_logged < 64 || prefs) {
		char name[32];
		proc_selfname(name, sizeof(name));
		if (prefs) {
			prefsCreates++;
		} else {
			sweet_surface_creates_logged++;
		}
		IOLog("SWEET SURF: %s create id %u %ux%u fmt 0x%x bpe %u bpr %u planes %u size %u keys %u -> 0x%x\\n",
		    name, s->id, s->width, s->height, s->pixelFormat, s->bytesPerElement,
		    s->bytesPerRow, s->planeCount, s->allocSize, props->getCount(), ret);
	}"""),
    ("""#if defined(FUXI)
 if (!((uintptr_t)options & kIOSurfaceLockReadOnly) && !sweet_gpu_writer(s, true)) {
  s->release(); return kIOReturnNotReady;
 }
#endif
	IOReturn ret = sweet_surface_seed_out(s, seedOut, false);
	s->release();
	return ret;""",
     """#if defined(FUXI)
 if (!((uintptr_t)options & kIOSurfaceLockReadOnly) && !sweet_gpu_writer(s, true)) {
  s->release(); return kIOReturnNotReady;
 }
#endif
	IOReturn ret = sweet_surface_seed_out(s, seedOut, false);
	static uint32_t prefsLocks;
	if (sweet_note(SN_LOCK) && prefsLocks < 60) {
		prefsLocks++;
		IOLog("SWEET SURF: Preferences lock %u options %#lx seed %u\\n", s->id, (unsigned long)(uintptr_t)options,
		    *sweet_surface_seed(s));
	}
	s->release();
	return ret;"""),
    ("""	IOReturn ret = sweet_surface_seed_out(s, seedOut,
	    !((uintptr_t)options & kIOSurfaceLockReadOnly));
	s->release();
	return ret;""",
     """	IOReturn ret = sweet_surface_seed_out(s, seedOut,
	    !((uintptr_t)options & kIOSurfaceLockReadOnly));
	static uint32_t prefsUnlocks;
	if (sweet_note((uintptr_t)options & kIOSurfaceLockReadOnly ? SN_UNLOCK_RO : SN_UNLOCK) && prefsUnlocks < 60) {
		prefsUnlocks++;
		IOLog("SWEET SURF: Preferences unlock %u options %#lx seed %u\\n", s->id, (unsigned long)(uintptr_t)options,
		    *sweet_surface_seed(s));
	}
	s->release();
	return ret;"""),
    ("""	if (sweet_surface_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, lock, &sweet_surface_lock)) {
			IOLockFree(lock);
		}
	}
""",
     """	if (sweet_surface_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, lock, &sweet_surface_lock)) {
			IOLockFree(lock);
		}
	}
	if (sweet_note_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, lock, &sweet_note_lock)) {
			IOLockFree(lock);
		}
	}
"""),
    # 0310's use log becomes these counts
    ("""	static struct {
		char name[20];
		uint32_t inc, dec, missed;
	} who[8];
	static uint32_t calls;
	char name[20];
	proc_selfname(name, sizeof(name));
	IOLockLock(sweet_surface_lock);
	const uint32_t n = ++calls;
	unsigned i = 0;
	while (i < 8 && who[i].name[0] != 0 && strncmp(who[i].name, name, sizeof(name)) != 0) {
		i++;
	}
	if (i == 8) {
		i = 7;                          /* the last row takes everyone else */
	} else if (who[i].name[0] == 0) {
		strlcpy(who[i].name, name, sizeof(who[i].name));
	}
	if (how != 1) {
		who[i].missed++;
	} else if (delta > 0) {
		who[i].inc++;
	} else {
		who[i].dec++;
	}
	if (n <= 40) {
		IOLog("SWEET SURF: use %u: %s surface %u %+d -> %s, its uses %u\\n", n, name, id, delta,
		    how == 0 ? "not its surface" : how == 1 ? "counted" : "nothing to drop", uses);
	}
	if (n % 1000 == 0) {
		for (unsigned k = 0; k < 8 && who[k].name[0] != 0; k++) {
			IOLog("SWEET SURF: uses after %u calls: %s +%u -%u missed %u\\n", n, who[k].name, who[k].inc, who[k].dec,
			    who[k].missed);
		}
	}
	IOLockUnlock(sweet_surface_lock);""",
     """	(void)id;
	(void)uses;
	(void)sweet_note(how != 1 ? SN_USE_MISSED : delta > 0 ? SN_USE_INC : SN_USE_DEC);"""),
])
print('patched (0311 surface counts)')
