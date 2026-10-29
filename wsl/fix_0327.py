W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0327: IBs whose buffer handle is no longer live ------------------------------------------------------------
# post21: backboardd's submission was refused (an IB in no buffer of the client) and its Metal device was lost. The
# user client now passes the handle Turnip gives for each IB's buffer; a handle no longer live, or whose buffer is
# not where the IB is, is logged with what holds that address now and when the handle's buffer was freed.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
copy('PandoraAdrenoGpu.cpp', 'iokit/Kernel/arm/PandoraAdrenoGpu.cpp')
