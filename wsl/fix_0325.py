W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0325: the IB1 of a GPU hang walked as it is in memory ----------------------------------------------------
# post15, post18 and post19 hung the same way: the BR in an IB2 whose address has no upper half (0xed42000, 0,
# 0x5362000), an SMMU fault on it, an opcode error. The hang dump now lists every IB call and suspicious draw state
# group of the IB1 as the memory holds it now, the page-table leaf of the IB1 next to its pool page, and the
# dwords at the IB2 address with the IB1's upper half: a writer that put the short address there, or a CP that
# read something else than the memory holds.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
