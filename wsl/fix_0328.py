W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0328: the context entry on every change of submitting client ----------------------------------------------
# a6xx_set_pagetable() emits the context entry whenever the submitting context differs from the ring's last one;
# ours only did on a client's first submission, while backboardd and the posters take turns on the ring (UniXNU
# Adreno740 8ef8b84 found hangs from the same gap). Off with pandora_gpu_ctxsw=0.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
