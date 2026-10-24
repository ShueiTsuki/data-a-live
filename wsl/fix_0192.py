W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60])
    open(path, 'w').write(s.replace(old, new))


# A frame rendered by the GPU is shown only once the GPU has finished it: Core Animation swaps
# after the command buffer is scheduled, not completed (real displays wait on the GPU's fence).
# The fence issued at the swap is recorded; the vsync copy waits for it (no copy that tick).
copy('pandora_gpu.c', 'bsd/dev/pandora_gpu.c')
F = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
edit(F, 'extern "C" void flush_dcache(vm_offset_t addr, unsigned count, int phys);\n',
     'extern "C" void flush_dcache(vm_offset_t addr, unsigned count, int phys);\n'
     '#include <sys/errno.h>\n'
     'extern "C" uint32_t pandora_gpu_issued(void);                         /* bsd/dev/pandora_gpu.c */\n'
     'extern "C" int pandora_gpu_wait(uint64_t fence, uint64_t timeout_ns);\n')
edit(F, '\tuint32_t      fGpuBusyTicks, fGpuFailed;\n',
     '\tuint32_t      fGpuBusyTicks, fGpuFailed;\n'
     '\tuint32_t      fPendingSeq;    /* PANDORA: GPU fence issued when fPending was swapped (0: none) */\n'
     '\tbool          fGpuDeferred;   /* PANDORA: the last flush tick waited for that fence */\n'
     '\tuint32_t      fStatGpuWait;   /* PANDORA: flush ticks that waited for the GPU */\n')
edit(F, '\tmemcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);\n\tfStatSwaps++;\n',
     '\tmemcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);\n'
     '#if defined(PANDORA)\n'
     '\tfPendingSeq = sweetfb_gpu_frames() ? pandora_gpu_issued() : 0;\n'
     '#endif\n'
     '\tfStatSwaps++;\n')
edit(F, '\tbool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;\n',
     '\tbool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;\n'
     '#if defined(PANDORA)\n'
     '\t/* A frame the GPU has not finished waits; it is shown on the first tick after. */\n'
     '\tflush = fb->fDirty && ((count % fb->fFlushTicks) == 0 || fb->fGpuDeferred);\n'
     '\tif (flush && fb->fPendingSeq && pandora_gpu_wait(fb->fPendingSeq, 0) == ETIMEDOUT) {\n'
     '\t\tflush = false;\n'
     '\t\tif (!fb->fGpuDeferred) {\n'
     '\t\t\tfb->fStatGpuWait++;\n'
     '\t\t}\n'
     '\t\tfb->fGpuDeferred = true;\n'
     '\t} else if (flush) {\n'
     '\t\tfb->fGpuDeferred = false;\n'
     '\t}\n'
     '#endif\n')
edit(F, '\t\tuint32_t swaps = fStatSwaps, dropped = fStatDropped;\n\t\tfStatSwaps = fStatDropped = 0;\n',
     '\t\tuint32_t swaps = fStatSwaps, dropped = fStatDropped, gpuWait = fStatGpuWait;\n'
     '\t\tfStatSwaps = fStatDropped = fStatGpuWait = 0;\n')
edit(F, '\t\t    "%u swaps, %u not shown\\n", fStatFrames, span_ns / 1000000, avg_ns / 1000,\n\t\t    max_ns / 1000, swaps, dropped);\n',
     '\t\t    "%u swaps, %u not shown, %u waited for the GPU\\n", fStatFrames, span_ns / 1000000,\n'
     '\t\t    avg_ns / 1000, max_ns / 1000, swaps, dropped, gpuWait);\n')
print('patched')
