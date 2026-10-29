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
patch('config/MASTER.arm64.pandora', [
    ("#  VM_BASE =        [ vm_pressure_events memorystatus config_code_decryption encrypted_swap config_deferred_reclaim ]",
     "#  VM_BASE =        [ vm_pressure_events jetsam memorystatus config_code_decryption encrypted_swap config_deferred_reclaim ]"),
])
