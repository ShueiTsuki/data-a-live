W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0262: the speaker graph on the audio DSP ------------------------------------------------------------------
# User 2026-10-28: thermal step 2 paused, speakers next. spk5: the amps' Halo cores loaded but idle, as if unclocked;
# their PLL locks to TDM0's bit clock, which runs only once the DSP's speaker graph plays (spkstart). pandora_spk=3
# opens Android's speaker graph in Android's order (pandora_adsp.c: GRAPH_OPEN and the calibration from spkgraph.bin,
# tools/spk_graph.py from the phone's ACDB; AGM's and PAL's run-time settings; PREPARE, START), runs it 12 s with
# silence and closes it. pandora_amp_tdm meanwhile writes Android's amp configuration, enables the serial port,
# powers the amps up and down as cs35l43_main_amp_event does, and logs PLL, interrupt and Halo state; no PLAY.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
copy('pandora_amp.c', 'bsd/dev/pandora_amp.c')
print('patched (speaker graph)')
