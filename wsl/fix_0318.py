W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0318: BoFree waits for the client's own submissions only ------------------------------------------------------
# post11: BoFree waited for every client's submissions so far. backboardd frees about 20 buffers a second and each
# free also waited for the posters' frames; Turnip frees while holding the lock its next allocation takes, so the
# Astronomy poster's main thread waited on that lock in 116 of 127 snapshots.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
