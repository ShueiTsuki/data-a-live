W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


# ---- 0304: the receiver's TDM interface ----------------------------------------------------------------------------
# aud33 (k0303): play-and-record went to the receiver (route Receiver, the receiver graph and the top amp in receiver
# mode up), but nothing played: the receiver graph's TDM tag data from the ACDB names interface 1 (the secondary TDM;
# the speaker's names 0). AGM fills in the backend's LPAIF type and interface index (configure_tdm_ep); now so does
# pandora_adsp.c, with TDM-LPAIF-RX-PRIMARY's.
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
print('patched (0304 receiver TDM interface)')
