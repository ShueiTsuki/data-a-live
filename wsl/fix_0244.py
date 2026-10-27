W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0244: log the first periodic gauge read ------------------------------------------------------------------------
# bat3: the 20 s reads only log every 15th; log the first too, so a daily boot shows they run.
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (first periodic gauge read logged)')
