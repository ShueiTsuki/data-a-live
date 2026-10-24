W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# No clean and invalidate of every imported page on every submission (NativeMetal flushes the
# surfaces each command buffer uses; boot-arg pandora_gpu_flushall=1 brings the loop back), and
# "PANDORA GPU SUBMIT" timing per 500 submissions (bb21: 27 ms from commit to completion).
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
print('patched')
