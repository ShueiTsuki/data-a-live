W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# GPU frequency scaling as Linux msm_devfreq with simple_ondemand on a8xx_gpu_busy: the GMU's busy
# counter every 50 ms, HFI GX_BW_PERF_VOTE on a change, a "PANDORA GPU DCVS" line every 10 s.
# pandora_gpu_perf (0196) still fixes the level and turns this off; pandora_gpu_perf_max caps it.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
print('patched')
