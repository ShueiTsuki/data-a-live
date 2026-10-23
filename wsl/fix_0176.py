W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'

# Adreno 840 bring-up, the buffer pool reserved before launchd (level 13 with
# its pool taken early; bsd/dev/pandora_gpu.c, pandora_gpu=13).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
