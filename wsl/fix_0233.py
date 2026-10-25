W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0233: the apps SMMU as the audio DSP's buffers will meet it, read only, and a 64 KiB buffer below 4 GiB -------
# Before the speakers: the DSP reads the PCM periods through the apps SMMU on its audio streams (DT msm-audio-ion
# 0x1001/0x1081, 0x1041/0x1061). The links' thread logs what the boot loaders left for those streams (stream-match
# groups, S2CRs, the banks they and the last bank use) and keeps a 64 KiB physically contiguous buffer below 4 GiB.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (SMMU survey for the audio streams)')
