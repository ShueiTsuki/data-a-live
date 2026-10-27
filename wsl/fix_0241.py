W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0241: the gauge read as Android sends it ----------------------------------------------------------------------
# gpicheck's live i2c-5 log: the command byte goes in an immediate TRE (type 0x11, data in dword 0), the write GO has
# rx_len 0, the config TRE is 0x0316050b / clk_div 2, and the transfers run on GPII1. pandora_gpi.c sends writes of up
# to 8 bytes that way; pa_gauge_read uses Android's clock and tries GPII1 first.
copy('pandora_gpi.c', 'bsd/dev/pandora_gpi.c')
copy('pandora_gpi.h', 'bsd/dev/pandora_gpi.h')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (gauge read as Android)')
