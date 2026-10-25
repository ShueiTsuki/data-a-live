/*
 * Patch 0232: the IPCC's interrupt line, the doorbells of the remote processors bsd/dev/pandora_adsp.c
 * talks to (ADSP, SOCCP, AOP). The phone's DT qcom,ipcc@1106000 gives <GIC_SPI 246 IRQ_TYPE_LEVEL_HIGH>
 * = INTID 278; tools/mkadt_pandora.py puts it on /arm-io/ipcc. Taken as PandoraAdrenoGpu.cpp takes the
 * GPU's: the nub through the GIC controller (SweetGIC.cpp), a filter at interrupt time. The filter's
 * pandora_ipcc_irq takes and clears what rang and wakes the links' thread; nothing is left for the work
 * loop. pandora_adsp.c enables the source once its IPCC window is mapped.
 */
#include <pexpert/arm64/board_config.h>
#if defined(PANDORA)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread.h>

extern "C" {
int pandora_ipcc_irq(void);
void pandora_ipcc_irq_attach(void (*enable)(void));
bool sweet_gic_ready(void);
}

class PandoraIpcc : public IOService {
	OSDeclareDefaultStructors(PandoraIpcc);

	bool irqFilter(IOFilterInterruptEventSource *source);
	void irqAction(IOInterruptEventSource *source, int count);

public:
	void startInterrupt(void);
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(PandoraIpcc, IOService);

static IOFilterInterruptEventSource *gPandoraIpccIrq;
static IOWorkLoop *gPandoraIpccLoop;

static void
pandora_ipcc_irq_enable(void)
{
	if (gPandoraIpccIrq != nullptr) {
		gPandoraIpccIrq->enable();
	}
}

bool
PandoraIpcc::irqFilter(IOFilterInterruptEventSource *source)
{
	if (pandora_ipcc_irq() != 0) {
		source->disable();   /* nothing pending, again and again: the links poll from now on */
	}
	return false;
}

void
PandoraIpcc::irqAction(IOInterruptEventSource *, int)
{
}

void
PandoraIpcc::startInterrupt(void)
{
	/* As PandoraAdrenoGpu::startInterrupt: this matches on IOResources and may start before the GIC
	 * controller and the /arm-io nubs exist. */
	OSDictionary *want = IOService::serviceMatching("SweetGIC");
	IOService *gic = want == nullptr ? nullptr : IOService::waitForMatchingService(want, 10ull * NSEC_PER_SEC);
	OSSafeReleaseNULL(want);
	want = IOService::nameMatching("ipcc");
	IOService *nub = want == nullptr ? nullptr : IOService::waitForMatchingService(want, 10ull * NSEC_PER_SEC);
	OSSafeReleaseNULL(want);
	OSSafeReleaseNULL(gic);
	if (nub == nullptr || !sweet_gic_ready() || nub->getProperty("IOInterruptSpecifiers") == nullptr) {
		IOLog("PANDORA IPCC: no interrupt (node %d, controller %d): the links poll\n", nub != nullptr,
		    sweet_gic_ready());
		OSSafeReleaseNULL(nub);
		return;
	}
	gPandoraIpccLoop = IOWorkLoop::workLoop();
	gPandoraIpccIrq = gPandoraIpccLoop == nullptr ? nullptr : IOFilterInterruptEventSource::filterInterruptEventSource(this,
	    OSMemberFunctionCast(IOInterruptEventSource::Action, this, &PandoraIpcc::irqAction),
	    OSMemberFunctionCast(IOFilterInterruptEventSource::Filter, this, &PandoraIpcc::irqFilter), nub, 0);
	if (gPandoraIpccIrq == nullptr || gPandoraIpccLoop->addEventSource(gPandoraIpccIrq) != kIOReturnSuccess) {
		IOLog("PANDORA IPCC: interrupt source refused: the links poll\n");
		OSSafeReleaseNULL(gPandoraIpccIrq);
		OSSafeReleaseNULL(nub);
		return;
	}
	gPandoraIpccIrq->disable();
	IOLog("PANDORA IPCC: interrupt source on %s, enabled when the links are mapped\n", nub->getName());
	pandora_ipcc_irq_attach(pandora_ipcc_irq_enable);
	OSSafeReleaseNULL(nub);
}

static void
pandora_ipcc_irq_thread(void *arg, wait_result_t)
{
	static_cast<PandoraIpcc *>(arg)->startInterrupt();
}

bool
PandoraIpcc::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	thread_t thread;
	if (kernel_thread_start(pandora_ipcc_irq_thread, this, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return true;
}
#endif /* PANDORA */
