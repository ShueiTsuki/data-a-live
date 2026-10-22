def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


patch('libkern/libkern/c++/OSMetaClass.h', [
    ("""#ifdef XNU_KERNEL_PRIVATE

#if !XNU_TARGET_OS_OSX
#define APPLE_KEXT_VTABLE_PADDING   0""",
     """#ifdef XNU_KERNEL_PRIVATE

/*
 * PANDORA builds with the macOS SDK (XNU_TARGET_OS_OSX) but runs original iOS kexts
 * prelinked from the iPhone X kernelcache (tools/mkprelink.py). They call our IOKit and
 * libkern objects by vtable slot, so our classes need iOS's vtable layout: no reserved
 * padding slots. Container alignment is already the same (1) on both.
 */
#if !XNU_TARGET_OS_OSX || defined(PANDORA)
#define APPLE_KEXT_VTABLE_PADDING   0"""),
])
print('patched')
