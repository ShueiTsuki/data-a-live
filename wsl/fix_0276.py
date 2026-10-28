W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0276: the speaker graph's stream volume control at unity, unmuted -----------------------------------------------
# spk17b (k0275b): the beeps were consumed by the speaker graph, both amps played with clock, rate and enables as in
# Android, but their firmware read AUDIO_IN_MAX 0 (Android during playback 0x109): silence on the TDM. The TDM lane
# (PARAM_ID_TDM_LANE_CFG 2) and interface match the ACDB. The stream's MODULE_ID_VOL_CTRL (0x401f) has no calibration
# in the ACDB; PAL sets the stream volume at stream start and Linux q6apm sets unity (0x2000, Q13). pandora_adsp.c now
# sets PARAM_ID_VOL_CTRL_MASTER_GAIN 0x2000 and _MUTE 0 there, and _MUTE 0 on the device volume control (0x437e).
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (0276 stream volume unity)')
