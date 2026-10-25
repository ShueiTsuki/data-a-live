W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0234: speaker groundwork, Xiaomi's battery messages, amp I2C block transfers ----------------------------
# pandora_spk=1: the apps SMMU passes the DSP's audio streams through (Linux arm-smmu-qcom's bypass handling), the
# buffer the loader keeps below 4 GiB (ADT /chosen pandora-dsp-shm) is mapped for the DSP by the APM and unmapped;
# pandora_spk=2 also: the TDM0 pins and the PRM's primary-TDM bit clock as Android requests it, held 2 s, released.
# The SOCCP is read with Xiaomi's MCA messages (276/280 bytes) instead of the classic battmgr ones (bat2: the
# answers came back in MCA form); values are logged only. pandora_amp: block reads and writes over the amps' I2C.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (speaker groundwork, MCA battery reads, amp I2C blocks)')
