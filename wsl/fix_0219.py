W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0219: 4 GiB GPU address windows (bsd/dev/pandora_gpu.c) ----------------------------------------------------
# r2g: still flickering after 0218; the frames that flashed iOS's boot logo were frames CoreAnimation could not draw:
# backboardd's Metal device refused textures, surface imports and its 1125x2436 frame buffers (OutOfMemory(Device),
# 3439 times from 44 s on; r2f 241 from 206 s). Turnip (tu_knl_darwin) takes GPU addresses from the client's window,
# and in the 32-bit layout only the first client to open gets 1 GiB, the others 178 MiB: vktest opened first,
# backboardd got 178 MiB, and 0218's busy answers had CoreAnimation add two more 11 MiB frame buffers. The GPU
# context now takes 36-bit addresses (the SMMU's upstream bus carries 49, ID2 0x5511) and each client gets 4 GiB
# from 4 GiB * (client + 1), as msm's private address spaces start at 0x1_0000_0000; the GMU stays at 32 bits.
# The watch log shows each window's mapped bytes and the pool in use.
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
print('patched (GPU windows)')
