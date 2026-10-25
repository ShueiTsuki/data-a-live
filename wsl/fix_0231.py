W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0231: the SOCCP link goes on from the boot loaders' GLINK session, after a ping ---------------------------------
# bat1: zeroing our FIFO's head while the SOCCP's read index stayed at 8 left no room in the FIFO, so VERSION never
# went out and the SOCCP's 16 waiting bytes could not be answered (DeepSeek's soccpcheck, checked against the phone's
# qcom_glink_smem.ko). For a remote that runs already our head now goes on from its read index and our read index
# stays; before GLINK starts the SOCCP is pinged as the phone's adsp_attach does (master-kernel bit 8, pong bit 9).
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (SOCCP GLINK session and ping)')
