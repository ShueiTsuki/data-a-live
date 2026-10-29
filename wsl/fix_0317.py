W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0317: thread snapshots of every process of a name, returns 8 to a line -----------------------------------------
# The Astronomy poster renders at about 15 frames a second while it uses a fifth of a core and the GPU is not busy
# (post10): it waits each frame. pandora_stacks now takes every running process of the name (the poster runs as a
# preview and an editor), and the returns go 8 to a line so the console's line length no longer cuts the app's
# own frames (post3's lines ended mid-stack).
copy('pandora_prof.c', 'osfmk/arm/pandora_prof.c')
