W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0227: one SCM call at a time, and queued calls resumed ---------------------------------------------------------
# adsp1: the ADSP thread's start-up queries and the GPU's GPU_INIT_REGS met in TrustZone at 0.18 s; the GPU's call
# came back WAITQ_SLEEP (x0 2), nothing resumed it, and every later SCM call of the boot came back 2 (the GPU stopped
# at level 3, the ADSP at init_image). scm_smc now holds a mutex around each call, as Linux's qcom_scm_lock did, and
# a call the firmware queues anyway is resumed as Linux's waitq code does (GET_WQ_CTX polled in place of the IRQ).
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (SCM lock and waitq resume)')
