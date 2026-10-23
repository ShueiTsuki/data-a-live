W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'
# Adreno 840 bring-up, level 9: the GPU's SMMU context, ring, SQE and scratch
# (bsd/dev/pandora_gpu.c, boot-arg pandora_gpu=9).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
