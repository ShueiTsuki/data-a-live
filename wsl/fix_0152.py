def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# 0151 checked `amfi != NULL`, but sweet_amfi_register runs at EARLY_BOOT, before
# mac_policy_initmach loads the security extensions, so the original AMFI then hit
# "AppleMobileFileIntegrity interface already set". Decide by the kernel image instead:
# tools/mkprelink.py always prelinks the original AMFI, so a non-empty __PRELINK_TEXT
# means it will register the interface itself.
patch('bsd/kern/sweet_amfi.c', [
    ("""	if (amfi != NULL) {
		/* The prelinked original AMFI registered first (PANDORA, tools/mkprelink.py). */
		printf("SWEET AMFI: original AMFI interface present; built-in not registered\\n");
		return;
	}""",
     """	kernel_segment_command_t *prelink = getsegbyname("__PRELINK_TEXT");
	if (amfi != NULL || (prelink != NULL && prelink->vmsize != 0)) {
		/* Original kexts are prelinked (PANDORA, tools/mkprelink.py, always with AMFI):
		 * the original AMFI registers the interface while the security extensions load. */
		printf("SWEET AMFI: original AMFI prelinked; built-in not registered\\n");
		return;
	}"""),
    ("""/* True when the built-in interface is the one in use (no original AMFI). */""",
     """#include <mach-o/loader.h>
#include <libkern/kernel_mach_header.h>

/* True when the built-in interface is the one in use (no original AMFI). */"""),
])
print('patched')
