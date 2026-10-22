def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# AMFI's developer-mode latch thread starts while the security extensions load (in
# mac_policy_initmach) and waits at most 30 s for ApplePMGRNub. 0148 started the nub
# publisher in bsd_init, much later: in QEMU the wait ran out first ("AMFI: No PMGR?").
# Start it just before the security extensions load; IOKit is up by then and the thread
# publishes the nub within 100 ms of the ApplePMGR kext's classes appearing.
patch('bsd/kern/bsd_init.c', [
    ("""#if defined(PANDORA)
	/* ApplePMGRNub for a prelinked original AMFI (iokit/Kernel/arm/PandoraPMGRNub.cpp). */
	extern void pandora_pmgr_init(void);
	pandora_pmgr_init();
#endif

""", ""),
])
patch('osfmk/kern/startup.c', [
    ("""#if CONFIG_MACF
	kernel_bootstrap_log("mac_policy_initmach");
	mac_policy_initmach();""",
     """#if defined(PANDORA)
	/* ApplePMGRNub for a prelinked original AMFI (iokit/Kernel/arm/PandoraPMGRNub.cpp):
	 * started before the security extensions load, as AMFI waits only 30 s for it. */
	extern void pandora_pmgr_init(void);
	pandora_pmgr_init();
#endif

#if CONFIG_MACF
	kernel_bootstrap_log("mac_policy_initmach");
	mac_policy_initmach();"""),
])
print('patched')
