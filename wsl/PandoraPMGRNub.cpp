/*
 * PANDORA (patch 0148): publish an ApplePMGRNub for Apple's original AMFI, prelinked from
 * the iPhone X iOS 16.7.16 kernelcache (tools/mkprelink.py, after UniXNU's NativePrelink).
 *
 * AMFI's developer-mode latch thread waits up to 30 s for a service of class ApplePMGRNub
 * and panics ("AMFI: No PMGR?") without one. It then reads the PMGR always-on register
 * through the nub, ORs in 4 and writes the value back. The nub's read and write forward to
 * the provider stored at nub+0x88 (vtable slots 312/313); with no provider the original
 * code returns 0xffffffff and does nothing on write, which is also what the generic
 * ApplePMGR class does (20H392 ApplePMGR.kext, read 0xfffffff005685734, write 0xfffffff005683dbc).
 * The nub's own overrides (attachToChild, message, compareName, matchLocation) are safe
 * without a provider.
 *
 * So the nub here is the original class from the prelinked ApplePMGR kext, made by its
 * metaclass, with no provider and no hardware behind it. Nothing happens when the kernel
 * has no prelinked ApplePMGR (the class never appears). Boot-arg pandora_pmgr=0 turns it off.
 */
#include <pexpert/arm64/board_config.h>

#if defined(PANDORA)
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>

#define PMGR_NUB_SIZE      0x740
#define PMGR_NUB_PROVIDER  0x88

extern "C" void pandora_pmgr_init(void);

static const OSMetaClass *
pandora_pmgr_meta(void)
{
	const OSSymbol *name = OSSymbol::withCString("ApplePMGRNub");
	const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : NULL;
	OSSafeReleaseNULL(name);
	return meta;
}

static void
pandora_pmgr_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	const OSMetaClass *meta = NULL;
	/* The prelinked ApplePMGR kext is loaded with AMFI, its dependent. */
	for (int i = 0; i < 300 && !(meta = pandora_pmgr_meta()); i++) {
		IOSleep(100);
	}
	if (!meta) {
		return;
	}
	IOService *nub = NULL;
	OSObject *obj = NULL;
	IOService *platform = IOService::getPlatform();
	const char *why = NULL;
	if (meta->getClassSize() != PMGR_NUB_SIZE) {
		why = "unexpected class size";
	} else if (!platform) {
		why = "no platform expert";
	} else if (!(obj = meta->alloc()) || !(nub = OSDynamicCast(IOService, obj))) {
		why = "alloc failed";
	} else if (*(volatile uintptr_t *)((uintptr_t)nub + PMGR_NUB_PROVIDER) != 0) {
		why = "provider field not empty";
	} else if (!nub->init()) {
		why = "init failed";
	} else if (!nub->attach(platform)) {
		why = "attach failed";
	}
	if (why) {
		printf("PANDORA PMGR: %s\n", why);
		OSSafeReleaseNULL(obj);
		return;
	}
	nub->setName("pandora-pmgr");
	nub->registerService(kIOServiceSynchronous);
	/* The registry holds it from here on; AMFI keeps using it for the whole boot. */
	printf("PANDORA PMGR: ApplePMGRNub published (no provider: reads 0xffffffff, writes ignored)\n");
}

void
pandora_pmgr_init(void)
{
	int on = 1;
	(void)PE_parse_boot_argn("pandora_pmgr", &on, sizeof(on));
	if (!on) {
		return;
	}
	thread_t thread;
	if (kernel_thread_start(pandora_pmgr_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
