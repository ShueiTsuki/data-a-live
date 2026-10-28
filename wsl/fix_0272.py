W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0272: the test tone with DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2 ------------------------------------------------
# spk14 (k0271): calibration, ALLOW_ACT_BOOT and the music scene went in, both amps played (AUDIO_STATE 2) and paused;
# but the shared-memory endpoint (0x4001) answered every version-1 data buffer command with GPR_IBASIC_RSP_RESULT
# status 3 (AR_EUNSUPPORTED), so no tone. gsl writes DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2 (0x0400100A, 11 words, the
# metadata ones 0) and gets DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2 (0x05001004); pandora_adsp.c now does the
# same and logs refusals.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (0272 tone with the v2 data command)')
