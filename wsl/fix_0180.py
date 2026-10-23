W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'

# Adreno 840: the carved GPU pool windows from ml_io_map_unmappable, so ml_io_unmap works (ADT /chosen
# pandora-gpu-pool, mkadt_pandora.py --gpu-pool), mapped write-combined; bsd/dev/pandora_gpu.c.
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
