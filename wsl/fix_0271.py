W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0271: the first sound: calibration, the scene on the running DSP, a quiet test tone ---------------------------
# spk13 (k0270): both amps' firmware awake, PLAY gives AUDIO_STATE 2, PAUSE 0; the audio DSP sent silence.
# pandora_amp.c: this phone's calibration (crus_calr.bin from Android's persist partition, copied into the pack only,
# never into the repo, values never printed) goes to CAL_R_SEL before the first PLAY, with 0x10110 bit 31 cleared and
# ALLOW_ACT_BOOT = 1 (cs35l43_dsp_audio_ev); the music scene is laid on the running DSP when the main amp powers up
# (cs35l43_main_amp_event -> cs35l43_apply_delta_tuning -> cs_dsp_load_coeff).
# pandora_adsp.c: a 1 kHz tone at -30 dBFS into the speaker graph's shared-memory endpoint while the graph runs
# (DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER, 20 ms buffers, four in flight); audible while the amps play.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0271 calibration, scene, test tone)')
