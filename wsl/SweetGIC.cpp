/*
 * PANDORA (patch 0216): the GIC as IOKit's interrupt controller for device interrupts.
 *
 * On an iPhone the AIC driver registers as the platform's interrupt controller and every driver takes its
 * interrupts through IOInterruptEventSource on its device tree nub ("interrupts" + "interrupt-parent");
 * IODeviceTreeSupport turns those into IOInterruptControllers / IOInterruptSpecifiers when the tree is read.
 * Here the GIC's distributor is that controller: SweetPlatformExpert creates this object for /arm-io/gic
 * (#interrupt-cells 2: INTID, Linux trigger flags), registers it under IODTInterruptControllerName and
 * installs its handleInterrupt on every CPU. sleh_irq (SWEET) already takes Group 1 interrupts: the timer and
 * IPIs itself, every other INTID through the CPU's interrupt handler, then writes EOIR.
 *
 * Shared peripheral interrupts only (INTID 32-1019), all level-sensitive (the phone's GPU and display lines
 * are IRQ_TYPE_LEVEL_HIGH), routed to the boot CPU. The configuration follows Linux irq-gic-v3 for an SPI:
 * Group 1 non-secure, priority 0xa0, level, IROUTER = target affinity, enable through ISENABLER, disable
 * through ICENABLER and wait for GICD_CTLR.RWP. IOInterruptController's protocol: a driver's disable is
 * soft; when the line fires again while soft-disabled, handleInterrupt disables it in the distributor.
 */
#include <pexpert/arm64/board_config.h>
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>

#if defined(PANDORA)
extern "C" vm_offset_t sweet_gicd_base(void);   /* pexpert/arm/pe_fiq.c */
extern "C" void pandora_install_interrupt_handler_all(void *nub, int source, void *target,
    IOInterruptHandler handler, void *refCon);   /* osfmk/arm64/machine_routines.c */
bool sweet_gic_create(IOService *platform, IORegistryEntry *node);   /* SweetPlatformExpert.cpp */
extern "C" bool sweet_gic_ready(void);                               /* PandoraAdrenoGpu.cpp */
extern "C" void sweet_gic_counts(uint64_t *fired, uint64_t *unregistered, uint64_t *hard_disables);

#define GIC_VECTORS       1020u
#define GIC_FIRST_SPI     32u
#define GICD_CTLR         0x0000u
#define GICD_CTLR_RWP     (1u << 31)
#define GICD_IGROUPR      0x0080u
#define GICD_ISENABLER    0x0100u
#define GICD_ICENABLER    0x0180u
#define GICD_ISPENDR      0x0200u
#define GICD_ICPENDR      0x0280u
#define GICD_IPRIORITYR   0x0400u
#define GICD_ICFGR        0x0c00u
#define GICD_IROUTER      0x6000u

class SweetGIC : public IOInterruptController {
	OSDeclareDefaultStructors(SweetGIC);

	vm_offset_t gicd = 0;
	uint64_t    route = 0;      /* IROUTER value: the boot CPU's affinity, IRM 0 */

	uint32_t rd(uint32_t o) const { return *(volatile uint32_t *)(gicd + o); }
	void wr(uint32_t o, uint32_t v) const { *(volatile uint32_t *)(gicd + o) = v; }
	void waitRWP() const;

public:
	/* Counts for the log (bsd/dev/pandora_gpu.c prints them with the GPU's). */
	uint64_t    fired = 0, unregistered = 0, hardDisables = 0;

	bool setup(IOService *platform, IORegistryEntry *node);
	IOReturn handleInterrupt(void *refCon, IOService *nub, int source) APPLE_KEXT_OVERRIDE;
	bool vectorCanBeShared(IOInterruptVectorNumber n, IOInterruptVector *v) APPLE_KEXT_OVERRIDE;
	void initVector(IOInterruptVectorNumber n, IOInterruptVector *v) APPLE_KEXT_OVERRIDE;
	int getVectorType(IOInterruptVectorNumber n, IOInterruptVector *v) APPLE_KEXT_OVERRIDE;
	void disableVectorHard(IOInterruptVectorNumber n, IOInterruptVector *v) APPLE_KEXT_OVERRIDE;
	void enableVector(IOInterruptVectorNumber n, IOInterruptVector *v) APPLE_KEXT_OVERRIDE;
	void causeVector(IOInterruptVectorNumber n, IOInterruptVector *v) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetGIC, IOInterruptController);

static SweetGIC *gSweetGIC;

void
SweetGIC::waitRWP() const
{
	for (unsigned i = 0; i < 100000 && (rd(GICD_CTLR) & GICD_CTLR_RWP); i++) {
	}
}

bool
SweetGIC::setup(IOService *platform, IORegistryEntry *node)
{
	gicd = sweet_gicd_base();
	const ml_topology_info_t *t = ml_get_topology_info();
	if (!gicd || !t || !t->boot_cpu) {
		IOLog("SWEET GIC: no distributor mapping or boot CPU\n");
		return false;
	}
	const uint64_t mpidr = t->boot_cpu->phys_id;
	route = (mpidr & 0x00ffffffull) | ((mpidr & 0xff00000000ull));   /* Aff3 at [39:32], Aff2..Aff0 at [23:0] */
	vectors = IONewZero(IOInterruptVector, GIC_VECTORS);
	if (!vectors) {
		return false;
	}
	for (unsigned i = 0; i < GIC_VECTORS; i++) {
		vectors[i].interruptLock = IOLockAlloc();
		if (!vectors[i].interruptLock) {
			return false;
		}
	}
	const OSSymbol *name = IODTInterruptControllerName(node);
	if (!name) {
		return false;
	}
	pandora_install_interrupt_handler_all(this, 0, this,
	    OSMemberFunctionCast(IOInterruptHandler, this, &SweetGIC::handleInterrupt), NULL);
	IOPlatformExpert *pe = OSDynamicCast(IOPlatformExpert, platform);
	if (!pe) {
		name->release();
		return false;
	}
	pe->registerInterruptController(const_cast<OSSymbol *>(name), this);
	IOLog("SWEET GIC: interrupt controller %s, SPIs %u-%u to MPIDR %#llx, %u lines in the distributor\n",
	    name->getCStringNoCopy(), GIC_FIRST_SPI, GIC_VECTORS - 1, mpidr, 32u * ((rd(0x4) & 0x1f) + 1));
	name->release();
	__atomic_store_n(&gSweetGIC, this, __ATOMIC_RELEASE);
	return true;
}

IOReturn
SweetGIC::handleInterrupt(void * /*refCon*/, IOService * /*nub*/, int source)
{
	if (source < (int)GIC_FIRST_SPI || source >= (int)GIC_VECTORS) {
		return kIOReturnInvalid;
	}
	IOInterruptVector *v = &vectors[source];
	fired++;
	if (!v->interruptRegistered) {
		unregistered++;
		disableVectorHard(source, v);   /* nobody to clear the source: keep the line quiet */
		return kIOReturnInvalid;
	}
	v->interruptActive = 1;
	OSMemoryBarrier();
	if (!v->interruptDisabledSoft) {
		v->handler(v->target, v->refCon, v->nub, v->source);
	} else if (!v->interruptDisabledHard) {
		/* the driver disabled it (IOInterruptEventSource does for a level line until its work loop runs) */
		v->interruptDisabledHard = 1;
		hardDisables++;
		disableVectorHard(source, v);
	}
	v->interruptActive = 0;
	return kIOReturnSuccess;
}

bool
SweetGIC::vectorCanBeShared(IOInterruptVectorNumber, IOInterruptVector *)
{
	return false;
}

void
SweetGIC::initVector(IOInterruptVectorNumber n, IOInterruptVector *)
{
	if (n < GIC_FIRST_SPI || n >= GIC_VECTORS) {
		return;
	}
	const uint32_t bit = 1u << (n % 32), word = 4 * (n / 32);
	wr(GICD_ICENABLER + word, bit);
	waitRWP();
	wr(GICD_IGROUPR + word, rd(GICD_IGROUPR + word) | bit);        /* Group 1 NS (WI if the secure side owns it) */
	*(volatile uint8_t *)(gicd + GICD_IPRIORITYR + n) = 0xa0;
	const uint32_t cfg = GICD_ICFGR + 4 * (n / 16), shift = 2 * (n % 16) + 1;
	wr(cfg, rd(cfg) & ~(1u << shift));                              /* level-sensitive */
	*(volatile uint64_t *)(gicd + GICD_IROUTER + 8 * n) = route;
	wr(GICD_ICPENDR + word, bit);
}

int
SweetGIC::getVectorType(IOInterruptVectorNumber, IOInterruptVector *)
{
	return kIOInterruptTypeLevel;
}

void
SweetGIC::disableVectorHard(IOInterruptVectorNumber n, IOInterruptVector *)
{
	if (n >= GIC_FIRST_SPI && n < GIC_VECTORS) {
		wr(GICD_ICENABLER + 4 * (n / 32), 1u << (n % 32));
		waitRWP();
	}
}

void
SweetGIC::enableVector(IOInterruptVectorNumber n, IOInterruptVector *)
{
	if (n >= GIC_FIRST_SPI && n < GIC_VECTORS) {
		wr(GICD_ISENABLER + 4 * (n / 32), 1u << (n % 32));
	}
}

void
SweetGIC::causeVector(IOInterruptVectorNumber n, IOInterruptVector *)
{
	if (n >= GIC_FIRST_SPI && n < GIC_VECTORS) {
		wr(GICD_ISPENDR + 4 * (n / 32), 1u << (n % 32));
	}
}

/* For SweetPlatformExpert::processTopLevel: create and register the controller for the /arm-io/gic node. */
bool
sweet_gic_create(IOService *platform, IORegistryEntry *node)
{
	SweetGIC *gic = OSTypeAlloc(SweetGIC);
	if (!gic || !gic->init()) {
		OSSafeReleaseNULL(gic);
		return false;
	}
	gic->setName("gic");
	if (!gic->attach(platform) || !gic->setup(platform, node)) {
		IOLog("SWEET GIC: setup failed; device interrupts stay off\n");
		return false;
	}
	gic->registerService();
	return true;
}

/* Whether device interrupts can be registered, and the controller's counts (for logs). */
extern "C" bool
sweet_gic_ready(void)
{
	return __atomic_load_n(&gSweetGIC, __ATOMIC_ACQUIRE) != NULL;
}

extern "C" void
sweet_gic_counts(uint64_t *fired, uint64_t *unregistered, uint64_t *hard_disables)
{
	SweetGIC *g = __atomic_load_n(&gSweetGIC, __ATOMIC_ACQUIRE);
	*fired = g ? g->fired : 0;
	*unregistered = g ? g->unregistered : 0;
	*hard_disables = g ? g->hardDisables : 0;
}
#endif /* PANDORA */
