W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'
# Adreno 840 bring-up, level 5 ("4b"): GMU cold start per Linux a6xx_gmu_fw_start on the a8xx
# path (bsd/dev/pandora_gpu.c, boot-arg pandora_gpu=5; gen80200_gmu.bin from the firmware pack).
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
print('patched')
