W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0243: the battery state as Android's battery psy ----------------------------------------------------------------
# battui: full is online + 100 % + charging done (FC), not FC alone; temperature is 0x06 (0.1 K) less 2730.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (battery state as Android)')
