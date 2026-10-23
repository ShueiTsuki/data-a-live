def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# Half-drawn frames while scrolling (fb1 dump: top of the home screen drawn, the rest black;
# still screens are fine). SwapEnd only stored the record and returned; the vsync timer copied
# the surface up to a few ticks later. backboardd then took the return as "buffer free" and
# cleared and redrew it while it still waited to be copied, so the copy caught a frame being
# drawn. Now SwapEnd waits until the previous frame has been copied (at most 100 ms, not
# while the display is off), so a buffer backboardd draws into again has been shown already.
# Frames that were drawn and then dropped (349 of 949 in amfi2) are not drawn any more.
patch('iokit/Kernel/arm/SweetFramebuffer.cpp', [
    ("""	bool          fDirty;
""",
     """	bool          fDirty;
	bool          fCopying;     /* the vsync timer is copying the frame it took from fPending */
"""),
    ("""	IOLockLock(fLock);
	memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);
	fStatSwaps++;""",
     """	IOLockLock(fLock);
#if defined(PANDORA)
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
	fStatSwaps++;"""),
    ("""			memcpy(rec, fb->fPending, SWEET_FB_SWAP_REC_SIZE);
			fb->fDirty = false;
		}
	}
	IOLockUnlock(fb->fLock);
	if (rec != NULL) {
		fb->present(rec);
		IOFreeData(rec, SWEET_FB_SWAP_REC_SIZE);
	}""",
     """			memcpy(rec, fb->fPending, SWEET_FB_SWAP_REC_SIZE);
			fb->fDirty = false;
			fb->fCopying = true;
		}
	}
	IOLockUnlock(fb->fLock);
	if (rec != NULL) {
		fb->present(rec);
		IOFreeData(rec, SWEET_FB_SWAP_REC_SIZE);
		IOLockLock(fb->fLock);
		fb->fCopying = false;
		IOLockWakeup(fb->fLock, &fb->fCopying, false);
		IOLockUnlock(fb->fLock);
	}"""),
])
print('patched')
