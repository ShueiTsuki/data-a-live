#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
def sub(p, pairs):
    t = open(p).read()
    for old, new in pairs:
        assert t.count(old) == 1, (p, old)
        t = t.replace(old, new)
    open(p, 'w').write(t)

sub('pexpert/pexpert/arm64/SWEET.h', [(
"""#if defined(FUXI)
/* Seven of the eight cores""",
"""#if defined(PANDORA)
/* SM8850: all eight Oryon cores, MPIDR 0x000-0x500 and 0x10000/0x10100 (Aff2 = 1). */
#define SWEET_SMP_MAX_CPUS         8
#elif defined(FUXI)
/* Seven of the eight cores""")])

sub('iokit/Kernel/arm/SweetCPU.cpp', [
("""	if (cpu->phys_id > 0x700 || (cpu->phys_id & 0xff) != 0 ||
	    topology->num_cpus > SWEET_SMP_MAX_CPUS) {""",
"""#if defined(PANDORA)
	/* SM8850: Aff2:Aff1 = 0:0-5 and 1:0-1, Aff0 always 0 */
	if ((cpu->phys_id & ~0x00ffff00u) != 0 ||
	    topology->num_cpus > SWEET_SMP_MAX_CPUS) {
#else
	if (cpu->phys_id > 0x700 || (cpu->phys_id & 0xff) != 0 ||
	    topology->num_cpus > SWEET_SMP_MAX_CPUS) {
#endif"""),
("""	#if defined(FUXI)
    cpuName = OSSymbol::withCStringNoCopy("SM8550");""",
"""#if defined(PANDORA)
	cpuName = OSSymbol::withCStringNoCopy("SM8850");
#elif defined(FUXI)
    cpuName = OSSymbol::withCStringNoCopy("SM8550");"""),
("""__builtin_arm_rsr64("MPIDR_EL1") & 0xffffULL,""",
 """__builtin_arm_rsr64("MPIDR_EL1") & 0xffffffULL,"""),
])

sub('osfmk/arm64/start.s', [(
"""#if HAS_CLUSTER || defined(SWEET)
	and		x0, x15, #0xFFFF					// Full Aff1:Aff0 (SWEET: core in Aff1)""",
"""#if defined(PANDORA)
	and		x0, x15, #0xFFFFFF					// Aff2:Aff1:Aff0 (SM8850 prime cores: Aff2 = 1)
#elif HAS_CLUSTER || defined(SWEET)
	and		x0, x15, #0xFFFF					// Full Aff1:Aff0 (SWEET: core in Aff1)""")])

sub('osfmk/arm64/machine_routines.c', [(
"""	phys_id &= MPIDR_AFF1_MASK | MPIDR_AFF0_MASK;

	for (unsigned i = 0; i < topology_info.num_cpus; i++) {""",
"""#if defined(PANDORA)
	/* SM8850 prime cores differ from cores 0/1 only in Aff2. */
	phys_id &= MPIDR_AFF2_MASK | MPIDR_AFF1_MASK | MPIDR_AFF0_MASK;
#else
	phys_id &= MPIDR_AFF1_MASK | MPIDR_AFF0_MASK;
#endif

	for (unsigned i = 0; i < topology_info.num_cpus; i++) {""")])

sub('pexpert/arm/pe_fiq.c', [(
"""	// We only care about aff1 and aff0
	uint32_t phys_id = __builtin_arm_rsr64("MPIDR_EL1") & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);
""",
"""#if defined(PANDORA)
	/* SM8850 prime cores differ from cores 0/1 only in Aff2. */
#define PE_GICR_AFF_MASK (MPIDR_AFF2_MASK | MPIDR_AFF1_MASK | MPIDR_AFF0_MASK)
#else
	// We only care about aff1 and aff0
#define PE_GICR_AFF_MASK (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK)
#endif
	uint32_t phys_id = __builtin_arm_rsr64("MPIDR_EL1") & PE_GICR_AFF_MASK;
"""), (
"""		uint32_t aff_value = (uint32_t) (gicr_typer >> GICR_TYPER_AFFINITY_VALUE_SHIFT) & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);""",
"""		uint32_t aff_value = (uint32_t) (gicr_typer >> GICR_TYPER_AFFINITY_VALUE_SHIFT) & PE_GICR_AFF_MASK;""")])
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0113-pandora-smp-aff2-prime-cores.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0113
