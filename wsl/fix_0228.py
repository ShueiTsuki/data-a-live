W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0228: the ADSP's bus vote, Linux's start order, a longer wait for the ready bit on request ---------------------
# adsp2: both images authenticated and left reset, the GPU ran beside them, but in 5 s the DSP made no SMP2P item and
# rang no doorbell. qcom_q6v5_prepare votes the DSP's way to DDR (DT "rproc_ddr") before load_state; of the BCMs on
# that path only LP0 serves LPASS alone, so LP0 is voted to the maximum before the start and taken back at handover
# or on failure. The order is now Linux's (bus vote, load_state, cx/mx); load_state goes off on failure.
# pandora_adsp_wait=MS waits longer than 5 s for the ready bit; the SMEM items are listed after the wait.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (ADSP LP0 bus vote, start order)')
