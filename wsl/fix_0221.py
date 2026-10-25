W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- 0221: CTL0's frame start interrupt in place of polling for a taken frame ----------------------------------------
# r2i's read-only probe: INTR2 bit 9 (CTL0 start) sets at every refresh (14 in 100 ms). The ADT's disp0 node now names
# the MDSS line (phone DT qcom,mdss_mdp: <GIC_SPI 83 LEVEL_HIGH> = INTID 115, tools/mkadt_pandora.py), AppleCLCD takes it
# with a filter as PandoraAdrenoGpu takes the GPU's, and the present thread sleeps until the frame start that takes its
# frame (bsd/dev/pandora_dpu.c pandora_scanout_wait) instead of waking every 250 us for up to a refresh.
copy('pandora_dpu.c', 'bsd/dev/pandora_dpu.c')
F = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
edit(F, '''extern "C" bool pandora_scanout_flushed(void);''', '''extern "C" bool pandora_scanout_flushed(void);
extern "C" int pandora_scanout_irq(void);                        /* 0221 */
extern "C" void pandora_scanout_irq_attach(void (*enable)(void));
extern "C" bool pandora_scanout_wait(uint32_t want, uint64_t limit_ns);
extern "C" void pandora_scanout_irq_counts(uint64_t out[5]);''')
edit(F, '''#include <IOKit/IOTimerEventSource.h>
''', '''#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
''')
edit(F, '''	bool scanSettle(void);
	void presentLoop(void);''', '''	bool scanSettle(void);
	bool scanIrqFilter(IOFilterInterruptEventSource *source);
	void scanIrqAction(IOInterruptEventSource *source, int count);
	void presentLoop(void);''')
edit(F, '''#if defined(PANDORA)
static void
sweet_fb_present_thread(void *arg, wait_result_t)
{
	static_cast<AppleCLCD *>(arg)->presentLoop();
}
#endif''', '''#if defined(PANDORA)
static void
sweet_fb_present_thread(void *arg, wait_result_t)
{
	static_cast<AppleCLCD *>(arg)->presentLoop();
}

/* 0221: the display's interrupt (CTL0 frame start); bsd/dev/pandora_dpu.c enables it once the pipes are ours. */
static IOFilterInterruptEventSource *gSweetScanIrq;

static void
sweet_scan_irq_enable(void)
{
	if (gSweetScanIrq != NULL) {
		gSweetScanIrq->enable();
	}
}

bool
AppleCLCD::scanIrqFilter(IOFilterInterruptEventSource *source)
{
	if (pandora_scanout_irq() != 0) {
		source->disable();   /* the line stayed up for another MDSS block: polling from now on */
	}
	return false;
}

void
AppleCLCD::scanIrqAction(IOInterruptEventSource *, int)
{
}
#endif''')
edit(F, '''#if defined(PANDORA)
	{
		thread_t t;
		if (kernel_thread_start(sweet_fb_present_thread, this, &t) != KERN_SUCCESS) {''', '''#if defined(PANDORA)
	if (provider->getProperty("IOInterruptSpecifiers") != NULL) {
		IOFilterInterruptEventSource *irq = IOFilterInterruptEventSource::filterInterruptEventSource(this,
		    OSMemberFunctionCast(IOInterruptEventSource::Action, this, &AppleCLCD::scanIrqAction),
		    OSMemberFunctionCast(IOFilterInterruptEventSource::Filter, this, &AppleCLCD::scanIrqFilter), provider, 0);
		if (irq != NULL && fWorkLoop->addEventSource(irq) == kIOReturnSuccess) {
			irq->disable();
			gSweetScanIrq = irq;
			pandora_scanout_irq_attach(sweet_scan_irq_enable);
		} else {
			IOLog("SWEET FB: display interrupt source refused: frame starts by polling\\n");
			OSSafeReleaseNULL(irq);
		}
	} else {
		IOLog("SWEET FB: no display interrupt in the device tree: frame starts by polling\\n");
	}
	{
		thread_t t;
		if (kernel_thread_start(sweet_fb_present_thread, this, &t) != KERN_SUCCESS) {''')
edit(F, '''	const uint64_t t0 = mach_absolute_time();
	uint64_t limit;
	nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &limit);
	bool on;
	while (!(on = pandora_scanout_flushed() && pandora_scanout_current(want)) && mach_absolute_time() - t0 < limit) {
		delay(250);     /* a sleep, not a spin */
	}''', '''	const uint64_t t0 = mach_absolute_time();
	/* 0221: asleep until the frame start that takes it (an interrupt), polling every 250 us without one. */
	const bool on = pandora_scanout_wait(want, 50 * NSEC_PER_MSEC);''')
edit(F, '''		fScreenSum = fScreenMax = 0;
		fScreenCount = 0;
#endif''', '''		fScreenSum = fScreenMax = 0;
		fScreenCount = 0;
		uint64_t irq[5];
		pandora_scanout_irq_counts(irq);
		IOLog("SWEET FB: display interrupts %llu (%llu frame starts); frame waits slept %llu, taken without the "
		    "interrupt %llu, polled %llu\\n", irq[0], irq[1], irq[2], irq[3], irq[4]);
#endif''')
print('patched (frame start interrupt)')
