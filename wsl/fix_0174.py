W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'

# Adreno 840 bring-up, level 12: an indirect buffer (CP_INDIRECT_BUFFER) holding one
# CP_MEM_WRITE (bsd/dev/pandora_gpu.c, boot-arg pandora_gpu=12).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
