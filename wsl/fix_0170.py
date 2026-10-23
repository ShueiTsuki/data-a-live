W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'
# Adreno 840 bring-up, level 8 reordered as Linux: interrupts, GX vote, then OOB GPU_SET
# (bsd/dev/pandora_gpu.c, boot-arg pandora_gpu=8).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
