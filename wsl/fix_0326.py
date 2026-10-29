W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0326: IB memory written while the GPU can still read it -------------------------------------------------
# post20 (backboardd): the BR in an IB2 at 0, but the IB1 it was in holds no IB call now. Each submission now keeps
# its IB list (the first 64); a submission whose IBs overlap an IB of a still pending one (other than the same IB
# again) is logged. The hang dump names the submissions that held the BR's IB1, walks every IB of the stuck one,
# and reads the draw state (SDS) registers.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
