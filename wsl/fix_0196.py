W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# boot-args pandora_gpu_perf (1..18, default 1 = 160 MHz) and pandora_gpu_bw (1..11) for the GX vote
# (bb22: the GPU ran at 160 MHz and the lowest bandwidth all along), and the hang dump only for
# work already pending at the previous watch tick (bb22: a false dump).
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
print('patched')
