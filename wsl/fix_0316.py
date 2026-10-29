W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0316: GPU SMMU contexts process later accesses after a fault (SCTLR.HUPCF) --------------------------------------
# post6 and post9: a compute dispatch of the Astronomy poster read address 0; the context terminated it, and with
# HUPCF 0 every later access of the context too until the watch thread cleared the fault 5 s later, so the GPU
# could not fetch commands and hung (same IB position both times). Linux's arm-smmu-qcom sets HUPCF on the Adreno
# contexts for this reason.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
