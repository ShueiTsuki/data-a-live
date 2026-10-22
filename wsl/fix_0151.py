import subprocess

UNIXNU = '/mnt/d/DeepSeekHarness/claude/unixnu_review/src/kits/NativePrelink/patches/'


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# UniXNU NativePrelink early security initializer (BSD-2-Clause-Patent, UniXNU contributors):
# OSKext::start calls the original function named by _NativePrelinkEarlySecurityInit right
# after the kext's start, before MAC lockdown. Applies to xnu-8796 with an offset.
diff = open(UNIXNU + 'xnu-10063.141.1-security-init.patch', 'rb').read().replace(b'\r', b'')
subprocess.run(['patch', '-p1', '--no-backup-if-mismatch'], input=diff, check=True)
patch('libkern/c++/OSKext.cpp', [
    ("""	/* An explicitly linked security initializer must run with its real""",
     """	/* From UniXNU NativePrelink: on PANDORA (tools/mkprelink.py) this runs the original
	 * iOS 16 AMFI initializer that the original Sandbox kext would otherwise call while
	 * the security extensions load, so AMFI's MAC policy and amfi interface exist before
	 * trust_cache_runtime_init().
	 *
	 * An explicitly linked security initializer must run with its real"""),
])

# The SWEET built-in AMFI steps aside when the original one has already registered.
patch('bsd/kern/sweet_amfi.c', [
    ("""static void
sweet_amfi_register(void)
{
	amfi_interface_register(&sweet_amfi);""",
     """static void
sweet_amfi_register(void)
{
	if (amfi != NULL) {
		/* The prelinked original AMFI registered first (PANDORA, tools/mkprelink.py). */
		printf("SWEET AMFI: original AMFI interface present; built-in not registered\\n");
		return;
	}
	sweet_amfi_builtin = true;
	amfi_interface_register(&sweet_amfi);"""),
    ("""void
sweet_amfi_policy_register(void)
{
	int error = mac_policy_register(&sweet_amfi_mac_conf, &sweet_amfi_mac_handle, NULL);
	printf("SWEET AMFI: platform binary policy %s (%d)\\n",
	    error ? "NOT registered" : "registered", error);
	error = mac_policy_register""",
     """void
sweet_amfi_policy_register(void)
{
	int error = 0;
	if (sweet_amfi_builtin) {
		error = mac_policy_register(&sweet_amfi_mac_conf, &sweet_amfi_mac_handle, NULL);
		printf("SWEET AMFI: platform binary policy %s (%d)\\n",
		    error ? "NOT registered" : "registered", error);
	} else {
		printf("SWEET AMFI: original AMFI policy in use; built-in policy not registered\\n");
	}
	error = mac_policy_register"""),
    ("""__startup_func
static void
sweet_amfi_register(void)""",
     """/* True when the built-in interface is the one in use (no original AMFI). */
static SECURITY_READ_ONLY_LATE(bool) sweet_amfi_builtin;

__startup_func
static void
sweet_amfi_register(void)"""),
])
print('patched')
