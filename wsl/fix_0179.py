W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'

# Adreno 840: the carved GPU pool mapped in windows only while the kernel touches it (ADT /chosen
# pandora-gpu-pool, mkadt_pandora.py --gpu-pool), mapped write-combined; bsd/dev/pandora_gpu.c.
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
