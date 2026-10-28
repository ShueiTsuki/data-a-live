W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0256: the thermistor GPIOs in high impedance by default ------------------------------------------------------
# usb7: as found under iOS six of the seven thermistor GPIOs are on (EN_CTL 0x80); clearing xoagg's took its reading
# from 71.9 to 48.2 degrees in the same boot, next to xo's 47.7. The other five writes failed: pandora_spmi_write kept
# only two 4 KiB pages of read-write channels mapped and their APIDs sit on further pages; it keeps sixteen now.
# pandora_therm=2 clears the seven EN_CTLs before setting the channels up, as Android's pinctrl does at probe.
copy('pandora_therm.c', 'bsd/dev/pandora_therm.c')

K = 'bsd/dev/fuxi_keys.c'
edit(K, '''	static struct {
		uint32_t pa;
		vm_offset_t va;
	} page[2];''', '''	static struct {
		uint32_t pa;
		vm_offset_t va;
	} page[16];''')
edit(K, '''	for (unsigned i = 0; i < 2 && !va; i++) {''', '''	for (unsigned i = 0; i < 16 && !va; i++) {''')
print('patched (thermistor GPIOs in high impedance)')
