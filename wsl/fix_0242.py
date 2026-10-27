W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0242: the real battery in iOS ----------------------------------------------------------------------------------
# spk6 read the bq27z561 gauge over GPI. pandora_gauge=3 keeps reading it every 20 s (clocks voted only for the read;
# the GPII's mapping and ring memory reused, no allocation per read; quiet log). pandora_battery_get hands the gauge's
# percentage, voltage, current, temperature, mAh, cycles and FC to SweetBattery; charger connected from the SOCCP's
# bus_volt when pandora_battery=1 reads it, else from charge current.
copy('pandora_gpi.c', 'bsd/dev/pandora_gpi.c')
copy('pandora_gpi.h', 'bsd/dev/pandora_gpi.h')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_battery.h', 'bsd/dev/pandora_battery.h')
print('patched (gauge to the battery service)')
