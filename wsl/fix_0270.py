W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0270: the amps' Halo core started with Android's core control values; the mailbox handshake; PLAY/PAUSE --------
# A regmap trace of Android reloading the bottom amp's DSP ("B DSP1 Preload Switch" off and on, ftrace regmap events,
# kept in D:\DeepSeekHarness\ampfwcheck\amptrace, not in the repo) shows the same MPU writes as ours, no PLL forcing,
# and one difference: cs_dsp_halo_start_core writes 0x2BC1000 = 0x281, then 0x81 (update_bits on the regmap cache,
# which holds 0x80 from the stop). We read the register from the chip (0 after the soft reset) and wrote 0x201 / 0x001:
# bit 7 stayed clear and the core ran into MPU violations at once (spk8-spk12). Within 3 ms of Android's start the
# firmware raises the mailbox interrupt; the driver clears it, writes 0x13020 <- 0x02000003 and reads the mailbox
# ("AWAKE"). pandora_amp.c now writes 0x281 / 0x81, does that handshake, and in the TDM step (no reset or reload)
# configures, powers up and, with the firmware awake, sends PLAY and PAUSE (silence; no calibration yet).
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0270 core control 0x281/0x81, mailbox, PLAY/PAUSE)')
