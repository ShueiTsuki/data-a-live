W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'
# Adreno 840 bring-up, level 6 ("4c"): HFI transport and the a8xx perf table
# (bsd/dev/pandora_gpu.c, boot-arg pandora_gpu=6).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
