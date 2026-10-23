W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'

# Adreno 840 bring-up, level 13: a contiguous buffer pool, GPU mappings added to the live
# context, and submissions framed as Linux a7xx_submit (bsd/dev/pandora_gpu.c, pandora_gpu=13).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
