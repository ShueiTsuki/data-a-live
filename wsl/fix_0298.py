W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def patch(path, old, new, count=1):
    s = open(path).read()
    if s.count(old) != count:
        raise SystemExit(f'{path}: {s.count(old)} matches for {old[:60]!r}')
    open(path, 'w').write(s.replace(old, new))


# ---- 0298: the amps' power follows the speaker's IO ------------------------------------------------------------
# The user: "闲时给功放下电：安卓和iOS是怎么做的，如果也是这样就也这么做"; "可以，开始第3步". Android, traced on the phone
# (ftrace of both amps around a 0.3 s AudioTrack beep): AudioFlinger's standby 2 s after the sound, then the amps down
# at once; up again in about 5 ms each at the next sound. iOS: HAL stops the speaker's IO about 2 s after a sound.
# pandora_amp.c: after the first play (pandora_spk_live) the power thread follows the IO, with Android's writes in
# Android's order; pandora_amp_final takes the amps down before a power off or restart. PandoraAudio.cpp: startIO waits
# for the amps (up to 300 ms) before its clock starts, stopIO lets them go down; the root domain's power-off and
# restart message calls pandora_amp_final. sweet_ufs.c: the timed reset of the tests (pandora_reboot_after) too.
# pandora_adsp.c: the GLINK line only when the link's state changes and every 60 s, not for every doorbell.
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
copy('PandoraAudio.cpp', 'iokit/Kernel/arm/PandoraAudio.cpp')
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
patch('bsd/dev/sweet_ufs.c', '''extern void pandora_usb_final(void);
''', '''extern void pandora_usb_final(void);
extern void pandora_amp_final(const char *why);   /* bsd/dev/pandora_amp.c, 0298 */
''')
patch('bsd/dev/sweet_ufs.c', '''			printf("PANDORA: pandora_reboot_after=%u reached, resetting\\n", reboot_after);
			lck_mtx_lock(&sweet_ufs_lock);''', '''			printf("PANDORA: pandora_reboot_after=%u reached, resetting\\n", reboot_after);
			pandora_amp_final("pandora_reboot_after");      /* 0298: the speaker amps down first */
			lck_mtx_lock(&sweet_ufs_lock);''')
print('patched (0298 amp power follows the IO)')
