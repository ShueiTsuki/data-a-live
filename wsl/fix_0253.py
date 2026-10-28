import os

W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0253: the USB serial log's last bytes before a reset --------------------------------------------------------
# usb4: the serial log matched the ring byte for byte except the last two lines, written 1 ms before the reset (the
# thread hands bytes over every 5 ms). pl_psci_reset, which every reset goes through (the restart hook, panics
# included, and pandora_reboot_after), now first calls pandora_usb_final: the serial thread stops, the rest of the
# ring goes into the module, and the module sends it from the resetting CPU (new qcom_usb_debug_final_drain), for
# at most 2 s.
for d in ('include', 'kernel', 'soc', 'usb'):
    for name in sorted(os.listdir(W + 'qcomusb/' + d)):
        copy('qcomusb/%s/%s' % (d, name), 'bsd/dev/qcomusb/%s/%s' % (d, name))
copy('pandora_usb.c', 'bsd/dev/pandora_usb.c')
edit('bsd/dev/sweet_ufs.c', '''static void
pl_psci_reset(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000009ULL;     /* PSCI SYSTEM_RESET */''', '''#if defined(PANDORA)
extern void pandora_usb_final(void);
#endif

static void
pl_psci_reset(void)
{
#if defined(PANDORA)
	/* the USB serial log's last bytes (bsd/dev/pandora_usb.c; nothing without pandora_usb=2) */
	pandora_usb_final();
#endif
	register uint64_t x0 __asm__("x0") = 0x84000009ULL;     /* PSCI SYSTEM_RESET */''')
print('patched (USB serial drained before a reset)')
