W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- 0218: IOMFB's swap queue and SwapWait, frames to the display from a thread of their own ----------------------
# r2f: with scanout, swiping flickered. CoreAnimation (QuartzCore CA::WindowServer::IOMFBDisplay) draws into one of
# two frame buffers (surfaces 2 and 3 in turn) and reuses one only when IOMobileFramebufferSwapWait (sel 6) says the
# swap that last showed it is off the screen (can_update: options 0x80000000, the swap id the buffer was last swapped
# with; the swap before: 0x80000001, polled for at most 1 ms a frame by swap_wait_timeout). Sel 6 answered "done" at
# once, so the next frame was drawn into the buffer the pipes were still reading. iOS 16's IOMobileGraphicsFamily
# (iPhone10,3 kernelcache, swap_wait_gated): options bit 31 = do not wait (kIOReturnBusy), test 0 = the swap is not
# queued (+0x1310 list), not active (+0x1308, put on screen now) and not the last completed one (+0x1464, on screen);
# 1 = not queued and not active; 2 = nothing queued or active; 3/4 = whether the display is up; display off or torn
# down: done at once; no timeout: 5 s, then the swaps are cleared (kIOReturnAborted).
# So: a swap is queued (fPending), taken (active) by a thread, waits for the GPU work issued before it, goes to the
# pipes (or is copied), and is on screen once CTL0 latched it (flush bits clear, both pipes' current address): one
# panel refresh (8.3 ms at 120 Hz) after the GPU finished, not at the next 60 Hz tick. The vsync tick only notifies.
copy('pandora_dpu.c', 'bsd/dev/pandora_dpu.c')
F = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
edit(F, ''' *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h],''',
     ''' *   6  SwapWait [swap id, options, timeout ms] (PANDORA, 0218: as iOS 16's swap_wait_gated, swapWait()),
 *   7  GetID, 8 GetDisplaySize -> [w, h],''')
edit(F, '''extern "C" void pandora_scanout_counts(uint64_t *shows, uint64_t *faults);''',
     '''extern "C" void pandora_scanout_counts(uint64_t *shows, uint64_t *faults);
extern "C" bool pandora_scanout_flushed(void);''')
edit(F, '''extern "C" int pandora_gpu_wait(uint64_t fence, uint64_t timeout_ns);
''', '''extern "C" int pandora_gpu_wait(uint64_t fence, uint64_t timeout_ns);
#include <kern/thread.h>
''')
edit(F, '''	uint32_t      fStatGpuWait;   /* PANDORA: flush ticks that waited for the GPU */''',
     '''	uint32_t      fStatGpuWait;   /* PANDORA: frames that waited for their GPU work */''')
edit(F, '''	bool          fGpuDeferred;   /* PANDORA: the last flush tick waited for that fence */
''', '')
edit(F, '''	uint64_t      fScanClock;
	uint32_t      fScanFrames, fScanCopies, fScanRefused;
#endif
''', '''	uint64_t      fScanClock;
	uint32_t      fScanFrames, fScanCopies, fScanRefused;
	/* 0218: the swap queue (presentLoop). Queued: fPending while fDirty; active: taken, going on screen (fCopying);
	 * shown: on screen now. */
	uint32_t      fSwapQueued, fSwapActive, fSwapShown;
	bool          fOffDone;       /* display off: the panel black, the pipes on it */
	uint32_t      fSwapWaits, fSwapBusy, fSwapSlept, fSwapTimeouts, fSwapLogged;
	uint64_t      fLatchTime, fLatchMax;  /* flush to latch, absolute time */
	uint32_t      fLatchCount, fLatchLate;
#endif
''')
edit(F, '''	void scanPanel(void);
	void scanTick(void);
#endif
};''', '''	void scanPanel(void);
	void scanTick(void);
	bool scanSettle(void);
	void presentLoop(void);
	void displayOff(void);
	bool swapDone(uint32_t id, uint32_t type);
	IOReturn swapWait(uint32_t id, uint32_t options, uint32_t timeoutMs);
#endif
};''')
edit(F, '''bool
AppleCLCD::start(IOService *provider)
{''', '''#if defined(PANDORA)
static void
sweet_fb_present_thread(void *arg, wait_result_t)
{
	static_cast<AppleCLCD *>(arg)->presentLoop();
}
#endif

bool
AppleCLCD::start(IOService *provider)
{''')
edit(F, '''	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");''',
     '''#if defined(PANDORA)
	{
		thread_t t;
		if (kernel_thread_start(sweet_fb_present_thread, this, &t) != KERN_SUCCESS) {
			IOLog("SWEET FB: no present thread\\n");
			return false;
		}
		thread_deallocate(t);
	}
#endif
	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");''')

# The vsync tick: notifications only; what it did for frames is presentLoop's now.
edit(F, '''		0, 0,
	};

#if defined(FUXI)
 uint64_t elapsed = 0;''', '''		0, 0,
	};

#if defined(PANDORA)
	/* 0218: frames, the pipes and display off are the present thread's (presentLoop); the tick only notifies. */
	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		((IOMobileFramebufferUserClient *)fb->fClients->getObject(i))->notify(SWEET_FB_NOTIFY_VSYNC, values, 6);
	}
	IOLockUnlock(fb->fLock);
	sweet_fb_rearm(fb, sender);
	return;
#endif
#if defined(FUXI)
 uint64_t elapsed = 0;''')
edit(F, '''#if defined(PANDORA)
	fb->scanTick();
	if (fb->fPowerOff) {
		fb->scanPanel();   /* the fade below draws into the panel memory */
	}
#endif
''', '')
edit(F, '''	bool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;
#if defined(PANDORA)
	/* A frame the GPU has not finished waits; it is shown on the first tick after. */
	flush = fb->fDirty && ((count % fb->fFlushTicks) == 0 || fb->fGpuDeferred);
	if (flush && fb->fPendingSeq && pandora_gpu_wait(fb->fPendingSeq, 0) == ETIMEDOUT) {
		flush = false;
		if (!fb->fGpuDeferred) {
			fb->fStatGpuWait++;
		}
		fb->fGpuDeferred = true;
	} else if (flush) {
		fb->fGpuDeferred = false;
	}
#endif
''', '''	bool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;
''')

# swap(): one swap queued while the one before goes on screen.
edit(F, '''#if defined(PANDORA)
	if (fDirty || fCopying) {
		uint64_t deadline;
		nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &deadline);
		deadline += mach_absolute_time();
		while ((fDirty || fCopying) && !fPowerOff && mach_absolute_time() < deadline) {
			IOLockSleepDeadline(fLock, &fCopying, *(AbsoluteTime *)&deadline, THREAD_UNINT);
		}
	}
#endif
	memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);
#if defined(PANDORA)
	fPendingSeq = sweetfb_gpu_frames() ? pandora_gpu_issued() : 0;
#endif
	fStatSwaps++;
	if (fDirty) {
		fStatDropped++;         /* the previous frame was never shown */
	}
	fDirty = true;
	IOLockUnlock(fLock);''', '''#if defined(PANDORA)
	/* 0218: the queue holds one swap while the one before it goes on screen (presentLoop); the next one waits here
	 * for room, at most 100 ms, then takes its place (that one is not shown). */
	if (fDirty) {
		uint64_t deadline;
		nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &deadline);
		deadline += mach_absolute_time();
		while (fDirty && !fPowerOff && mach_absolute_time() < deadline) {
			IOLockSleepDeadline(fLock, &fDirty, *(AbsoluteTime *)&deadline, THREAD_UNINT);
		}
	}
#endif
	memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);
#if defined(PANDORA)
	fPendingSeq = sweetfb_gpu_frames() ? pandora_gpu_issued() : 0;
	fSwapQueued = get32(rec, 0x50);
#endif
	fStatSwaps++;
	if (fDirty) {
		fStatDropped++;         /* the previous frame was never shown */
	}
	fDirty = true;
#if defined(PANDORA)
	IOLockWakeup(fLock, &fDirty, false);         /* presentLoop */
	IOLockWakeup(fLock, &fSwapShown, false);     /* swapWait(): a swap taken over is done */
#endif
	IOLockUnlock(fLock);''')

edit(F, '''/* 0217: each vsync tick, drop what the pipes no longer read. */''',
     '''/* 0217: drop what the pipes no longer read (0218: scanSettle, once they latched a frame). */''')
edit(F, '''	if (fScanQueued != NULL && pandora_scanout_current(fScanQueuedAddr)) {
		sweet_scan_drop(&fScanShown);
		fScanShown = fScanQueued;
		fScanQueued = NULL;
	}
}
#endif
''', '''	if (fScanQueued != NULL && pandora_scanout_current(fScanQueuedAddr)) {
		sweet_scan_drop(&fScanShown);
		fScanShown = fScanQueued;
		fScanQueued = NULL;
	}
}

/*
 * 0218: after a frame went to the pipes (or they went back to the panel memory): until CTL0 has latched it (its flush
 * bits clear at the frame that takes them, one panel refresh at most: 8.3 ms at 120 Hz) and both pipes read it; then
 * the pipes no longer read the frame before it (scanTick lets that go). False when that took over 50 ms.
 */
bool
AppleCLCD::scanSettle(void)
{
	if (!fScanOn || (fScanQueued == NULL && !fScanToPanel)) {
		return true;
	}
	const uint32_t want = fScanToPanel ? 0 : fScanQueuedAddr;
	const uint64_t t0 = mach_absolute_time();
	uint64_t limit;
	nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &limit);
	bool on;
	while (!(on = pandora_scanout_flushed() && pandora_scanout_current(want)) && mach_absolute_time() - t0 < limit) {
		delay(250);     /* a sleep, not a spin */
	}
	const uint64_t took = mach_absolute_time() - t0;
	fLatchTime += took;
	fLatchMax = MAX(fLatchMax, took);
	fLatchCount++;
	if (!on) {
		fLatchLate++;
	}
	scanTick();
	return on;
}

/*
 * 0218: frames go to the display here, as through IOMFB's swap queue in iOS: a swap waits in fPending (queued) until
 * the one before it is on screen, is taken (active), waits for the GPU work issued before it (backboardd swaps once
 * its command buffer is scheduled, so that work includes the frame), goes to the pipes or is copied into the panel
 * memory, and is on screen (shown) once the pipes latched it. CoreAnimation draws into its two frame buffers in turn
 * and into one only when SwapWait says the swap that last showed it is off the screen (swapWait), so each swap has
 * to reach the screen soon after it came, not at a later tick. Display off is done here as well.
 */
void
AppleCLCD::presentLoop(void)
{
	uint8_t *rec = (uint8_t *)IOMallocData(SWEET_FB_SWAP_REC_SIZE);
	uint64_t gap = 0, last = 0;
	if (rec == NULL) {
		IOLog("SWEET FB: present thread: no memory\\n");
		return;
	}
	if (fFlushTicks > 1) {      /* sweetfb_hz under 60: at most that many frames a second */
		nanoseconds_to_absolutetime((uint64_t)fFlushTicks * SWEET_FB_VSYNC_US * NSEC_PER_USEC, &gap);
	}
	IOLockLock(fLock);
	for (;;) {
		if (fPowerOff && !fOffDone) {
			fOffDone = true;
			IOLockUnlock(fLock);
			displayOff();
			IOLockLock(fLock);
			continue;
		}
		if (fPowerOff || !fDirty) {
			IOLockSleep(fLock, &fDirty, THREAD_UNINT);
			continue;
		}
		memcpy(rec, fPending, SWEET_FB_SWAP_REC_SIZE);
		const uint32_t seq = fPendingSeq, id = fSwapQueued;
		fDirty = false;
		fCopying = true;
		fSwapActive = id;
		IOLockWakeup(fLock, &fDirty, false);        /* swap(): the queue has room */
		IOLockUnlock(fLock);
		if (gap != 0 && mach_absolute_time() < last + gap) {
			clock_delay_until(last + gap);
		}
		if (seq != 0 && pandora_gpu_wait(seq, 0) == ETIMEDOUT) {
			fStatGpuWait++;
			(void)pandora_gpu_wait(seq, 500 * NSEC_PER_MSEC);   /* at most 0.5 s */
		}
		present(rec);
		last = mach_absolute_time();
		(void)scanSettle();
		IOLockLock(fLock);
		fCopying = false;
		fSwapActive = 0;
		fSwapShown = id;
		IOLockWakeup(fLock, &fSwapShown, false);    /* swapWait() */
	}
}

/* 0218: display off (RequestPowerChange 0): the panel memory black (written only, it is write-combined), then the
 * pipes back on it if they read a surface: black on screen, and the surfaces are let go. */
void
AppleCLCD::displayOff(void)
{
	for (uint32_t y = 0; y < fHeight; y++) {
		volatile uint64_t *row = (volatile uint64_t *)(fPixels + (uint64_t)y * fRowBytes);
		for (uint32_t x = 0; x < fWidth / 2; x++) {
			row[x] = 0;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	scanPanel();
	(void)scanSettle();
}

/*
 * 0218: swap_wait_gated's tests, with the lock held. 0: the swap is off the screen (not queued, not active, not the
 * one shown); 1: it is on screen or was taken over (not queued, not active); 2: nothing queued or active. Display off:
 * all done (swap_wait_gated returns at once then).
 */
bool
AppleCLCD::swapDone(uint32_t id, uint32_t type)
{
	const bool queued = fDirty && id == fSwapQueued, active = fCopying && id == fSwapActive;
	if (fPowerOff) {
		return true;
	}
	switch (type) {
	case 0:
		return id == 0 || (!queued && !active && id != fSwapShown);
	case 1:
		return id == 0 || (!queued && !active);
	default:
		return !fDirty && !fCopying;
	}
}

/*
 * 0218: SwapWait (sel 6) [swap id, options, timeout ms] as swap_wait_gated: options bit 31 asks without waiting
 * (kIOReturnBusy while not done), the low bits are the test (swapDone; 3 and 4 ask whether the display is up: it is).
 * A wait ends at its timeout (kIOReturnTimeout) or, given none, after 5 s (kIOReturnAborted, where iOS clears its
 * swaps).
 */
IOReturn
AppleCLCD::swapWait(uint32_t id, uint32_t options, uint32_t timeoutMs)
{
	const uint32_t type = options & 0x7fffffffu;
	IOReturn ret = type == 4 || type > 4 ? kIOReturnBadArgument : kIOReturnSuccess;
	IOLockLock(fLock);
	fSwapWaits++;
	if (type <= 2 && !swapDone(id, type)) {
		if (options & 0x80000000u) {
			ret = kIOReturnBusy;
			fSwapBusy++;
		} else {
			uint64_t deadline;
			nanoseconds_to_absolutetime((uint64_t)(timeoutMs ? timeoutMs : 5000) * NSEC_PER_MSEC, &deadline);
			deadline += mach_absolute_time();
			fSwapSlept++;
			while (!swapDone(id, type)) {
				if (IOLockSleepDeadline(fLock, &fSwapShown, *(AbsoluteTime *)&deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
				    !swapDone(id, type)) {
					ret = timeoutMs ? kIOReturnTimeout : kIOReturnAborted;
					fSwapTimeouts++;
					break;
				}
			}
		}
	}
	const bool log = fSwapLogged < 24;
	const uint32_t queued = fDirty ? fSwapQueued : 0, active = fCopying ? fSwapActive : 0, shown = fSwapShown;
	fSwapLogged += log;
	IOLockUnlock(fLock);
	if (log) {
		IOLog("SWEET FB: swap wait %u options %#x timeout %u -> %#x (queued %u active %u shown %u)\\n", id, options,
		    timeoutMs, ret, queued, active, shown);
	}
	return ret;
}
#endif
''')

# Stats: the waits and how long the pipes took.
edit(F, '''		    fScanOn ? "on" : "off", fScanFrames, fScanCopies, scanShows, scanFaults, fScanRefused);
		fScanFrames = fScanCopies = 0;
#endif''', '''		    fScanOn ? "on" : "off", fScanFrames, fScanCopies, scanShows, scanFaults, fScanRefused);
		fScanFrames = fScanCopies = 0;
		IOLockLock(fLock);
		const uint32_t waits = fSwapWaits, busy = fSwapBusy, slept = fSwapSlept, late = fSwapTimeouts;
		fSwapWaits = fSwapBusy = fSwapSlept = fSwapTimeouts = 0;
		IOLockUnlock(fLock);
		uint64_t latchAvg = 0, latchMax = 0;
		absolutetime_to_nanoseconds(fLatchCount ? fLatchTime / fLatchCount : 0, &latchAvg);
		absolutetime_to_nanoseconds(fLatchMax, &latchMax);
		IOLog("SWEET FB: swap waits %u (%u busy, %u slept, %u timed out); pipes latched in avg %llu us max %llu us "
		    "(%u frames, %u over 50 ms)\\n", waits, busy, slept, late, latchAvg / 1000, latchMax / 1000, fLatchCount,
		    fLatchLate);
		fLatchTime = fLatchMax = 0;
		fLatchCount = fLatchLate = 0;
#endif''')

# Display on/off wakes the present thread; on: the redraw is the latest swap again.
edit(F, '''			if (off != fFB->fPowerOff) {
				fFB->fPowerOff = off;
				fFB->fFadeStep = 0;
				fFB->fDirty = true;     /* on: redraw the latest frame */
			}''', '''			if (off != fFB->fPowerOff) {
				fFB->fPowerOff = off;
				fFB->fFadeStep = 0;
				fFB->fDirty = true;     /* on: redraw the latest frame */
#if defined(PANDORA)
				fFB->fOffDone = false;
				fFB->fSwapQueued = get32(fFB->fPending, 0x50);
				IOLockWakeup(fFB->fLock, &fFB->fDirty, false);
				IOLockWakeup(fFB->fLock, &fFB->fSwapShown, false);
#endif
			}''')
edit(F, '''	case kSweetFBSwapWait:
	default:
		break;''', '''#if defined(PANDORA)
	case kSweetFBSwapWait:  /* kern_SwapWait / kern_SwapWaitWithTimeout: [swap id, options, timeout ms] */
		ret = fFB->swapWait(args->scalarInputCount > 0 ? (uint32_t)args->scalarInput[0] : 0,
		    args->scalarInputCount > 1 ? (uint32_t)args->scalarInput[1] : 0,
		    args->scalarInputCount > 2 ? (uint32_t)args->scalarInput[2] : 0);
		break;
#else
	case kSweetFBSwapWait:
#endif
	default:
		break;''')
print('patched (swap queue)')
