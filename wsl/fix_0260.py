W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0260: the battery and NAND sensors thermalmonitord waits for ------------------------------------------------
# usb10: thermalmonitord read the 30 sensors all along (798 copyEvents) but never computed: it runs its control only in
# a round where every sensor of its watchdog mask got a value, requiresWatchDog defaults to true, and TG0B, TG0V (the
# gas gauge's battery temperature, a HID service on the iPhone X too) and NAND CH0 temp had no service. They are
# published now: the gauge's reading for the first two, quiet_therm's for the NAND one (information only).
copy('pandora_therm.c', 'bsd/dev/pandora_therm.c')
print('patched (battery and NAND sensors for iOS)')
