#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/sweet_ufs.c'
t = open(p).read()

def sub(old, new, what):
    global t
    assert t.count(old) == 1, what
    t = t.replace(old, new)

# 1. The driver is ported now: drop the gate and its flag.
sub("""#if defined(PANDORA)
	if (!pandora_ufs_ported) {
		printf("PANDORA UFS: driver not ported to SM8850 yet\\n");
		return ENXIO;
	}
#endif
""", "", "gate")
i = t.index("/* Not ported to SM8850 yet:")
j = t.index("static bool pandora_ufs_ported = false;\n") + len("static bool pandora_ufs_ported = false;\n")
t = t[:i] + t[j:]

# 2. SM8550 GCC write and the fuxi_ufs_probe gate: not on PANDORA (clocks are on, 0116).
sub("""#if defined(FUXI)
    uint32_t requested=0;
    if (!PE_parse_boot_argn("fuxi_ufs_probe", &requested, sizeof(requested)) || requested!=1)
        return ENXIO;""", """#if defined(FUXI) && !defined(PANDORA)
    uint32_t requested=0;
    if (!PE_parse_boot_argn("fuxi_ufs_probe", &requested, sizeof(requested)) || requested!=1)
        return ENXIO;""", "gcc")

# 3. ADT node name.
sub("""#if defined(FUXI)
    const char *ufs_node="/fuxi-ufs";""", """#if defined(FUXI) && !defined(PANDORA)
    const char *ufs_node="/fuxi-ufs";""", "node")

# 4. DMA must be inside the SMMU bypass context's 36-bit output range (0114).
sub("""	su.dma_pa = ptoa_64(pn);
	su.lun = 0;
	su.secsize = DEV_BSIZE;

	lck_mtx_lock(&sweet_ufs_lock);
#if defined(FUXI)
	printf("FUXI UFS diagnostic""", """	su.dma_pa = ptoa_64(pn);
#if defined(PANDORA)
	if ((su.dma_pa + DMA_SIZE - 1) >> 36) {
		printf("PANDORA UFS: DMA at 0x%llx is outside the SMMU's 36-bit range\\n", su.dma_pa);
		return ENXIO;
	}
#endif
	su.lun = 0;
	su.secsize = DEV_BSIZE;

	lck_mtx_lock(&sweet_ufs_lock);
#if defined(FUXI)
	printf("FUXI UFS diagnostic""", "dma")

# 5. Wake the link before the ACTIVE power command (0118).
sub("""    error = ufs_take_over_host();
#if defined(FUXI)
    if (!error) {
        static const uint8_t active[6]""", """    error = ufs_take_over_host();
#if defined(PANDORA)
    if (!error) {
        error = pandora_h8_exit();
        printf("PANDORA UFS: hibernate exit: %x\\n", error);
    }
#endif
#if defined(FUXI)
    if (!error) {
        static const uint8_t active[6]""", "h8")

# 6. pandora_ufs_probe=3: the real init, then one READ of the partition's block 1.
test = open('/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/pandora_ufs_drvtest.c.txt').read()
sub("""#if defined(FUXI)
void fuxi_storage_probe(void);
""", test + """#if defined(FUXI)
void fuxi_storage_probe(void);
""", "test")
sub("""  if (probe==2) pandora_ufs_gpt();
""", """  if (probe==2) pandora_ufs_gpt();
  if (probe==3) pandora_ufs_driver_test();
""", "dispatch")
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0120-pandora-ufs-driver-init.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0120
