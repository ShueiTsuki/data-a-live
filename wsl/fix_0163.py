W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'
# Adreno 840 bring-up, level 2: CX GDSC on, gpu_cc_ahb and the SMMU vote clock on, then the
# level 1 SMMU and GMU reads again (bsd/dev/pandora_gpu.c, boot-arg pandora_gpu=2).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
