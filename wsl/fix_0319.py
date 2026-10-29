W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0319: pandora_stacks=NAME...:SECONDS ---------------------------------------------------------------------------
# With the speaker's boot-args the command line is 589 of its 607 characters; "pandora_stacks=AegirPoster
# pandora_stacks_every=2" did not fit (post11 ran without sound). The interval can now follow the names.
copy('pandora_prof.c', 'osfmk/arm/pandora_prof.c')
