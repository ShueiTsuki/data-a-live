W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0246: the battery as Android shows it -----------------------------------------------------------------------
# battui2: Android's percentage is remaining over full charge with 97 % stretched to 100 % (fg_ui_soc_smooth, the
# non-EU path, charging or not; DT soc-proportion-c 97), recomputed every 5 s and passed up only when it changes;
# the charger comes as the SOCCP's notification 0 with USB online in payload word 0. The gauge is read every 5 s,
# the notification sets the charger at once (a pass's bus_volt only between notifications).
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (battery as Android shows it)')
