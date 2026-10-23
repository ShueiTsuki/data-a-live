W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'

# GPU watch log with pandora_gpu_metal=1: per-client counts, the ring, SMMU faults
# (bsd/dev/pandora_gpu.c).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
