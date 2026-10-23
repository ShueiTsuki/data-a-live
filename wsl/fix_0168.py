W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'
# Adreno 840 bring-up, level 7: HFI bandwidth table, CORE_FW_START and START
# (bsd/dev/pandora_gpu.c, boot-arg pandora_gpu=7).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
