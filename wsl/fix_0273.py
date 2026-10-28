W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0273: the tone's packet buffer holds the 24-byte GPR header -----------------------------------------------------
# spk15 (k0272): no tone buffer went out ("sends refused 977"): the GPR header is 24 bytes (ShengGpr.c), the v2 data
# command 44, and the packet buffer was 64, so gpr_encode returned 0 every time (version 1, 52 bytes, had fitted).
# pandora_adsp.c: buffer sized from GPR_HEADER_BYTES; an encode failure now turns the tone off with a log line
# instead of counting as a busy channel.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (0273 tone packet size)')
