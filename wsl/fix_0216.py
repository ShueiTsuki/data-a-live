W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# Device interrupts through IOKit, as on an iPhone: the GIC distributor becomes the platform's interrupt
# controller (iokit/Kernel/arm/SweetGIC.cpp), created by the platform expert for /arm-io/gic; nubs carry
# "interrupts" + "interrupt-parent" (tools/mkadt_pandora.py) and drivers use IOInterruptEventSource.
# First user: the GPU's fence interrupt (PandoraAdrenoGpu.cpp, bsd/dev/pandora_gpu.c), so waits sleep
# until the GPU's interrupt instead of polling every 100 us.
copy('SweetGIC.cpp', 'iokit/Kernel/arm/SweetGIC.cpp')
copy('PandoraAdrenoGpu.cpp', 'iokit/Kernel/arm/PandoraAdrenoGpu.cpp')
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
edit('iokit/conf/files.arm64',
     'iokit/Kernel/arm/PandoraAdrenoGpu.cpp                            optional iokitcpp\n',
     'iokit/Kernel/arm/PandoraAdrenoGpu.cpp                            optional iokitcpp\n'
     'iokit/Kernel/arm/SweetGIC.cpp                                    optional iokitcpp\n')

edit('pexpert/arm/pe_fiq.c', '''static vm_offset_t gicd_base;
static vm_offset_t gicr_base;
static vm_offset_t gicr_size;
''', '''static vm_offset_t gicd_base;
static vm_offset_t gicr_base;
static vm_offset_t gicr_size;

#if defined(SWEET)
/* The distributor mapping, for iokit/Kernel/arm/SweetGIC.cpp (0216): it configures SPIs there. */
vm_offset_t sweet_gicd_base(void);
vm_offset_t
sweet_gicd_base(void)
{
	return gicd_base;
}
#endif
''')

edit('osfmk/arm64/machine_routines.c', '''/*
 *	Routine:        ml_init_interrupt
 *	Function:	Initialize Interrupts
 */''', '''#if defined(PANDORA)
/*
 * 0216: ml_install_interrupt_handler for every CPU at once. SWEET's sleh_irq hands every interrupt other than
 * the timer and IPIs to the CPU's interrupt handler; the GIC controller (iokit/Kernel/arm/SweetGIC.cpp) is
 * created after the CPUs have started, so it installs itself on all of them from one thread. The handler is
 * stored last, after the rest is visible.
 */
void pandora_install_interrupt_handler_all(void *nub, int source, void *target, IOInterruptHandler handler,
    void *refCon);
void
pandora_install_interrupt_handler_all(void *nub, int source, void *target, IOInterruptHandler handler,
    void *refCon)
{
	for (int cpu = 0; cpu <= ml_get_max_cpu_number(); cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);
		if (cdp == NULL) {
			continue;
		}
		cdp->interrupt_nub = nub;
		cdp->interrupt_source = source;
		cdp->interrupt_target = target;
		cdp->interrupt_refCon = refCon;
		__builtin_arm_dmb(DMB_ISH);
		cdp->interrupt_handler = handler;
	}
	__builtin_arm_dsb(DSB_ISH);
}
#endif

/*
 *	Routine:        ml_init_interrupt
 *	Function:	Initialize Interrupts
 */''')

S = 'iokit/Kernel/arm/SweetPlatformExpert.cpp'
edit(S, '''void
SweetPlatformExpert::processTopLevel(IORegistryEntry *root)
{''', '''#if defined(PANDORA)
extern bool sweet_gic_create(IOService *platform, IORegistryEntry *node);   /* SweetGIC.cpp (0216) */
#endif

void
SweetPlatformExpert::processTopLevel(IORegistryEntry *root)
{''')
edit(S, '''	IORegistryEntry *armio = root->childFromPath("arm-io", gIODTPlane);
	if (armio != NULL) {
		createNubs(this, IODTFindMatchingEntries(armio, kIODTExclusive, "('gic')"));
		armio->release();
	}''', '''	IORegistryEntry *armio = root->childFromPath("arm-io", gIODTPlane);
	if (armio != NULL) {
#if defined(PANDORA)
		/* 0216: the GIC as IOKit's interrupt controller for the nubs' "interrupts" (still no nub of its own). */
		IORegistryEntry *gic = armio->childFromPath("gic", gIODTPlane);
		if (gic != NULL) {
			(void)sweet_gic_create(this, gic);
			gic->release();
		}
#endif
		createNubs(this, IODTFindMatchingEntries(armio, kIODTExclusive, "('gic')"));
		armio->release();
	}''')
print('patched')
