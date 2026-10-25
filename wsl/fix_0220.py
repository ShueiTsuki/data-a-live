W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- 0220: 120 Hz for CoreAnimation, frame pacing numbers, touch count, read-only probes ---------------------------
# iOS side (QuartzCore / IOMobileFramebuffer of the iPhone10,3 16.7 cache): AppleDisplay reads the framebuffer
# service's "IOMFBDisplayRefresh" {displayMinRefreshInterval (seconds, 32.32 fixed), displayRefreshStepMachTime}
# through IORegistryEntrySearchCFProperty, and AppleCLCDDisplay::heartbeat_rate makes the shortest interval the period
# of CoreAnimation's own frame timer (CADeviceUseVBL is off for A11, ChipID 0x8015). Animations without a frame rate
# range get 80-120 Hz (CA::Render::Animation::next_time) unless the device claims APT (CADeviceSupportsAPT, a
# MobileGestalt answer): then 60-80, the cap users see on ProMotion phones (CA_FRAME_RATE_POLICY=2 lifts it there).
# No step is published, so APT stays off and the panel runs at a fixed 120 Hz.
copy('pandora_dpu.c', 'bsd/dev/pandora_dpu.c')                 # interrupt probe at scanout start
copy('pandora_cpu_idle.c', 'osfmk/arm/pandora_cpu_idle.c')     # PSCI feature probe
F = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
edit(F, '''	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
''', '''	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
#if defined(PANDORA)
	/*
	 * 0220: the panel's refresh for CoreAnimation: "IOMFBDisplayRefresh" {displayMinRefreshInterval: seconds as 32.32
	 * fixed point, displayRefreshStepMachTime: the variable-refresh step}. AppleDisplay reads it from this service and
	 * AppleCLCDDisplay::heartbeat_rate makes the shortest interval CoreAnimation's frame period when there is no step.
	 * Step 0: a fixed rate, and no APT claim (APT would put animations without a range at 60-80 Hz). Boot-arg
	 * sweetfb_refresh (Hz, default 120; 0 publishes nothing: 60 Hz, as the iPhone X).
	 */
	{
		uint32_t refresh = 120;
		PE_parse_boot_argn("sweetfb_refresh", &refresh, sizeof(refresh));
		OSDictionary *d = refresh ? OSDictionary::withCapacity(2) : NULL;
		OSNumber *interval = d ? OSNumber::withNumber((1ULL << 32) / refresh, 32) : NULL;
		OSNumber *step = d ? OSNumber::withNumber(0ULL, 32) : NULL;
		if (interval && step) {
			d->setObject("displayMinRefreshInterval", interval);
			d->setObject("displayRefreshStepMachTime", step);
			setProperty("IOMFBDisplayRefresh", d);
			IOLog("SWEET FB: IOMFBDisplayRefresh: %u Hz (displayMinRefreshInterval %llu)\\n", refresh, (1ULL << 32) / refresh);
		}
		OSSafeReleaseNULL(interval);
		OSSafeReleaseNULL(step);
		OSSafeReleaseNULL(d);
	}
#endif
''')
edit(F, '''	uint32_t      fLatchCount, fLatchLate;
''', '''	uint32_t      fLatchCount, fLatchLate;
	/* 0220: pacing while animating (swaps under 50 ms apart), and how long a swap takes to reach the screen. */
	uint64_t      fLastSwapAbs, fPendingAbs, fGapSum, fGapMin, fScreenSum, fScreenMax;
	uint32_t      fGapCount, fScreenCount;
''')
edit(F, '''	fSwapQueued = get32(rec, 0x50);
#endif''', '''	fSwapQueued = get32(rec, 0x50);
	{
		const uint64_t now = mach_absolute_time();
		uint64_t limit;
		nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &limit);
		if (fLastSwapAbs && now - fLastSwapAbs < limit) {
			const uint64_t gap = now - fLastSwapAbs;
			fGapSum += gap;
			fGapMin = fGapCount ? MIN(fGapMin, gap) : gap;
			fGapCount++;
		}
		fLastSwapAbs = now;
		fPendingAbs = now;
	}
#endif''')
edit(F, '''		const uint32_t seq = fPendingSeq, id = fSwapQueued;
		fDirty = false;''', '''		const uint32_t seq = fPendingSeq, id = fSwapQueued;
		const uint64_t swapAbs = fPendingAbs;
		fDirty = false;''')
edit(F, '''		present(rec);
		last = mach_absolute_time();
		(void)scanSettle();
''', '''		present(rec);
		last = mach_absolute_time();
		(void)scanSettle();
		if (swapAbs) {
			const uint64_t took = mach_absolute_time() - swapAbs;
			fScreenSum += took;
			fScreenMax = MAX(fScreenMax, took);
			fScreenCount++;
		}
''')
edit(F, '''		fLatchTime = fLatchMax = 0;
		fLatchCount = fLatchLate = 0;
#endif''', '''		fLatchTime = fLatchMax = 0;
		fLatchCount = fLatchLate = 0;
		IOLockLock(fLock);
		const uint64_t gapSum = fGapSum, gapMin = fGapMin;
		const uint32_t gaps = fGapCount;
		fGapSum = fGapMin = 0;
		fGapCount = 0;
		IOLockUnlock(fLock);
		uint64_t gapAvg = 0, gapShort = 0, screenAvg = 0, screenMax = 0;
		absolutetime_to_nanoseconds(gaps ? gapSum / gaps : 0, &gapAvg);
		absolutetime_to_nanoseconds(gapMin, &gapShort);
		absolutetime_to_nanoseconds(fScreenCount ? fScreenSum / fScreenCount : 0, &screenAvg);
		absolutetime_to_nanoseconds(fScreenMax, &screenMax);
		const uint64_t fps10 = gapAvg ? 10ULL * NSEC_PER_SEC / gapAvg : 0;
		IOLog("SWEET FB: while animating (swaps under 50 ms apart): %u, avg %llu us = %llu.%llu fps, shortest %llu us; "
		    "swap to screen avg %llu us max %llu us\\n", gaps, gapAvg / 1000, fps10 / 10, fps10 % 10, gapShort / 1000,
		    screenAvg / 1000, screenMax / 1000);
		fScreenSum = fScreenMax = 0;
		fScreenCount = 0;
#endif''')
edit(F, '''#else
	case kSweetFBSwapWait:
#endif
	default:
		break;
	}''', '''#else
	case kSweetFBSwapWait:
#endif
	default:
#if defined(PANDORA)
		/*
		 * 0220: what iOS passes to the selectors taken without effect (QuartzCore's IOMFBDisplay: 15 SetDebugFlags,
		 * 17 SetGammaTable, 22 SetDisplayDevice, 29 GetDisplayArea, 33 video power savings, 55 SetMatrix, 57
		 * GetColorRemapMode, 68 SetParameter, 74 SupportedFrameInfo, 78 SetBlock, 79 GetBlock, 84 SetContrast),
		 * the first 6 calls of each.
		 */
		if (selector < 96 && sweet_fb_arg_logs[selector] < 6) {
			char in[100];
			int at = 0;
			sweet_fb_arg_logs[selector]++;
			in[0] = 0;
			if (args->structureInputDescriptor == NULL && args->structureInput != NULL) {
				const uint8_t *p = (const uint8_t *)args->structureInput;
				for (uint32_t i = 0; i < args->structureInputSize && i < 48 && at < (int)sizeof(in) - 3; i++) {
					at += snprintf(in + at, sizeof(in) - at, "%02x", p[i]);
				}
			}
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET FB: %s sel %u scalars %u [%llx %llx %llx %llx] struct %u [%s] out %u/%u\\n", name, selector,
			    args->scalarInputCount, args->scalarInputCount > 0 ? args->scalarInput[0] : 0,
			    args->scalarInputCount > 1 ? args->scalarInput[1] : 0, args->scalarInputCount > 2 ? args->scalarInput[2] : 0,
			    args->scalarInputCount > 3 ? args->scalarInput[3] : 0, args->structureInputSize, in,
			    args->scalarOutputCount, args->structureOutputSize);
		}
#endif
		break;
	}''')
edit(F, '''				fFB->fSwapQueued = get32(fFB->fPending, 0x50);
''', '''				fFB->fSwapQueued = get32(fFB->fPending, 0x50);
				fFB->fPendingAbs = 0;          /* the redraw is not a new swap: no swap-to-screen time */
''')
edit(F, '''static uint64_t sweet_fb_logged;        /* selectors already logged (< 64) */''',
     '''static uint64_t sweet_fb_logged;        /* selectors already logged (< 64) */
#if defined(PANDORA)
static uint8_t sweet_fb_arg_logs[96];   /* 0220: calls of each unhandled selector logged with their arguments */
#endif''')

# Touch: the most fingers down at once, for the ten-point check.
H = 'iokit/Kernel/arm/hid/SweetHID.cpp'
edit(H, '''	IOFixed fX[SWEET_TOUCH_FINGERS];
	IOFixed fY[SWEET_TOUCH_FINGERS];
};
OSDefineMetaClassAndStructors(SweetHIDTouch, IOHIDEventService);''', '''	IOFixed fX[SWEET_TOUCH_FINGERS];
	IOFixed fY[SWEET_TOUCH_FINGERS];
	unsigned fMostDown;            /* PANDORA 0220: the most fingers down at once so far */
};
OSDefineMetaClassAndStructors(SweetHIDTouch, IOHIDEventService);''')
edit(H, '''	fDown[finger] = down;
	if (down) {
		fX[finger] = nx;
		fY[finger] = ny;
	}''', '''	fDown[finger] = down;
	if (down) {
		fX[finger] = nx;
		fY[finger] = ny;
	}
	unsigned downNow = 0;
	for (int i = 0; i < SWEET_TOUCH_FINGERS; i++) {
		downNow += fDown[i];
	}
	const bool most = downNow > fMostDown;
	if (most) {
		fMostDown = downNow;
	}''')
edit(H, '''	if (was != down) {
		IOLog("SWEET HID: touch %d %s at %d,%d%s\\n", finger, down ? "down" : "up", x, y,
		    fEdge[finger] ? " edge" : "");
	}''', '''	if (was != down) {
		IOLog("SWEET HID: touch %d %s at %d,%d%s\\n", finger, down ? "down" : "up", x, y,
		    fEdge[finger] ? " edge" : "");
	}
	if (most) {
		IOLog("SWEET HID: %u fingers down at once (the most so far)\\n", downNow);
	}''')
print('patched (120 Hz, pacing, touch count, probes)')
