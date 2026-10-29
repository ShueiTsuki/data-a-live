def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60], s.count(o))
        s = s.replace(o, n)
    open(p, 'w').write(s)


# ---- 0315: CONFIG_JETSAM, as the iPhoneOS configuration has it -------------------------------------------------------
# config/MASTER.arm64.pandora took its VM option set from MacOSX (0102): no jetsam. memorystatus_control's jetsam
# commands then answer EINVAL, and RunningBoard manages no process (post8: MEMORYSTATUS_CMD_CONVERT_MEMLIMIT_MB
# refused 203 times, "This process will not be managed" 145 times, memory limits, jetsam priorities, suspension and
# GPU priority all skipped). An unmanaged dirty-tracked process is not isApp(), so os_proc_available_memory answers
# 0: ClockKitUI then evicted and rebuilt the Astronomy poster's textures every frame. Without jetsam the kernel also
# frees memory the macOS way (swap files, then killing the process with the most compressed memory: launchd in
# post7). Only jetsam is added here; freeze, phantom_cache, secluded memory and the other iPhoneOS VM options stay
# off for now.
# Apple builds jetsam only for embedded targets; our kernel is a macOS-target build (XNU_TARGET_OS_OSX, blacktop's
# darwin-xnu-build with machine PANDORA), so code under CONFIG_JETSAM can use what only the !XNU_TARGET_OS_OSX
# branches define. CI run 56: vm_compressor_backing_store.c's app-swap file limits.
patch('osfmk/vm/vm_compressor_backing_store.c', [
    ("""#define VM_SWAP_SHOULD_TRIM(swf)        ((swf->swp_delayed_trim_count >= VM_SWAPFILE_DELAYED_TRIM_MAX) ? 1 : 0)

#endif /* !XNU_TARGET_OS_OSX */""",
     """#define VM_SWAP_SHOULD_TRIM(swf)        ((swf->swp_delayed_trim_count >= VM_SWAPFILE_DELAYED_TRIM_MAX) ? 1 : 0)

#if CONFIG_JETSAM
/* PANDORA (0315): jetsam on a macOS-target kernel; app swap's file limits as the embedded branch has them for a
 * device without ARM_LARGE_MEMORY. */
#define VM_MAX_SWAP_FILE_SWAP_ENABLED_NUM (4ULL * (1ULL << 30) / MAX_SWAP_FILE_SIZE)
#define VM_MIN_SWAP_FILE_SWAP_ENABLED_NUM (4ULL * (1ULL << 30) / MAX_SWAP_FILE_SIZE)
#define VM_SWAP_MIN_VOLUME_CAPACITY (128ULL * (1ULL << 30))
#endif /* CONFIG_JETSAM */

#endif /* !XNU_TARGET_OS_OSX */"""),
])

patch('config/MASTER.arm64.pandora', [
    ("#  VM_BASE =        [ vm_pressure_events memorystatus config_code_decryption encrypted_swap config_deferred_reclaim ]",
     "#  VM_BASE =        [ vm_pressure_events jetsam memorystatus config_code_decryption encrypted_swap config_deferred_reclaim ]"),
])
