W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0296: live play logs less ---------------------------------------------------------------------------------
# aud22 (k0295, pandora_spk_live=1): the graph stays up and the amps play, but "PANDORA ADSP: GPR in" for every 20 ms
# buffer flooded the kernel log (the device's per-second ring lines and the amps' readouts were lost). The GPR line is
# kept for the first 64 messages; live play logs its buffer counts every 10 s instead.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (0296 live play logs less)')
