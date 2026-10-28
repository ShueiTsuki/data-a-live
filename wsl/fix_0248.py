W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0248: the USB controller's handover state, read only ----------------------------------------------------------
# Before a USB serial console (QcomXnuUsbDebug's DWC3 CDC ACM) takes the controller: pandora_usb=1 logs the clocks,
# reset and power domain of ssusb@a600000 and, only when they are on, the DWC3 and QSCRATCH registers twice 100 ms
# apart. No writes.
copy('pandora_usb.c', 'bsd/dev/pandora_usb.c')
edit('bsd/conf/files', 'bsd/dev/pandora_therm.c\t\t\tstandard\n',
     'bsd/dev/pandora_therm.c\t\t\tstandard\nbsd/dev/pandora_usb.c\t\t\tstandard\n')
U = 'bsd/dev/sweet_ufs.c'
edit(U, '''extern void pandora_therm_init(void);
#endif''', '''extern void pandora_therm_init(void);
extern void pandora_usb_init(void);
#endif''')
edit(U, '''	pandora_therm_init();
#endif''', '''	pandora_therm_init();
	/* USB controller handover state, read only, boot-arg pandora_usb (bsd/dev/pandora_usb.c). */
	pandora_usb_init();
#endif''')
print('patched (USB handover state, read only)')
