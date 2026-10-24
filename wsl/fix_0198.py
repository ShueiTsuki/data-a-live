W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# The frequency governor rewritten after Linux msm_gpu_devfreq (50 % target, boost on activity after
# idle) and KGSL (short windows, a busy ceiling, 80 ms idle timeout, the DT's per-level bus-freq,
# initial 222 MHz): up at once, down one level after 4 windows, idle windows not counted (bb25:
# 0197 swung 160 <-> 1200 MHz up to 109 times in 10 s).
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
print('patched')
