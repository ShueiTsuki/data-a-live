W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0280: the top amp's receiver switch GPIO 174 high, as Android's music state ----------------------------------
# spk21 (k0279): each piece reached only its amp; the user cannot tell the earpiece from a speaker by ear. The top
# amp (0x42) has pinctrl rcv_gpio_high / rcv_gpio_low on GPIO 174; mixer "T Enable RCV Pin Control" is 1 (high) by
# default and 0 (low) only on the handset path; Android reads "gpio174 : out high func0 2mA pull up". We never set
# it. pandora_amp.c: the TLMM mapping up to GPIO 175, GPIO 174 set high in the TDM thread with its old state logged.
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (0280 GPIO 174 high)')
