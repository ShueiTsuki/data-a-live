W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0331: buffer slots handed out next-fit ------------------------------------------------------------------------
# Turnip's Darwin backend indexes its tu_bo structs by the kernel's buffer slot and zeroed a struct only after the
# BoFree of its slot; the lowest free slot went to the next allocation, which filled the same struct and had it
# zeroed under its owner (post23/26/27). tools/turnip/pandora_bo_finish_order.py fixes the order; next-fit slots keep
# a freed slot out of use for a while as well (NativeAdreno's handles come from a counter).
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
