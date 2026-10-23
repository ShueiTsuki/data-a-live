W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'

# GPU bring-up without the 3.5 s log-flush wait per step unless pandora_gpu_slow=1
# (bsd/dev/pandora_gpu.c).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
