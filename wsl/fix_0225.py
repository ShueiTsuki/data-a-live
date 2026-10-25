W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0225: the profiler logs dyld's image list with every report ---------------------------------------------------
# The log keeps its last 4 MiB only; a busy profiling run writes about 10 MB, and r2l/r2m lost the one image list
# logged at the first report, so none of their user PCs could be named.
copy('pandora_prof.c', 'osfmk/arm/pandora_prof.c')
print('patched (profiler image list every report)')
