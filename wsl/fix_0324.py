W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0324: GPU context faults logged as they happen ------------------------------------------------------------
# post18 froze: two SMMU faults reading address 0 (fsynr0 00180003, the Astronomy poster's old one), then a hang.
# The watch looks every 5 s, after the faulting job has retired. The DCVS thread (every 16 ms) now logs each new
# fault with its syndromes, UCHE_CLIENT_PF and the job then on the GPU.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
