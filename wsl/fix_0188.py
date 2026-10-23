# The framebuffer copy invalidates each source row from the CPU caches first when the GPU may
# have written the surface (boot-arg pandora_gpu_metal=1: a Metal client renders IOSurfaces
# through the non-cacheable GPU mapping; iokit/Kernel/arm/SweetFramebuffer.cpp).


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


patch('iokit/Kernel/arm/SweetFramebuffer.cpp', [
    ("""#include <sys/proc.h>
""", """#include <sys/proc.h>
#if defined(PANDORA)
extern "C" void flush_dcache(vm_offset_t addr, unsigned count, int phys);

/* Patch 0188: with a Metal client the GPU writes IOSurfaces non-cacheably (bsd/dev/pandora_gpu.c);
 * the copy below then cleans and invalidates each source row first, or it can copy stale lines. */
static bool
sweetfb_gpu_frames(void)
{
	static int on = -1;
	if (on < 0) {
		on = 0;
		PE_parse_boot_argn("pandora_gpu_metal", &on, sizeof(on));
	}
	return on != 0;
}
#endif
"""),
    ("""			if (copied != 2) for (int32_t y = 0; y < h; y++) {
				memcpy(""", """			if (copied != 2) for (int32_t y = 0; y < h; y++) {
#if defined(PANDORA)
				if (sweetfb_gpu_frames()) {
					flush_dcache((vm_offset_t)(src + s->offset + (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
					    (unsigned)w * 4, FALSE);
				}
#endif
				memcpy("""),
])
print('patched')
