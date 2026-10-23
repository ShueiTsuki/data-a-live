W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'

# Adreno 840: level 15, the rest of a8xx hw_init (GMEM range, UBWC, protection; ADT /chosen
# pandora-gpu-pool, mkadt_pandora.py --gpu-pool), mapped write-combined; bsd/dev/pandora_gpu.c.
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
