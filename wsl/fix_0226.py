W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- 0226: talking to the audio DSP: SMP2P ready and handover, GLINK, the GPR channel and the APM question ----------
# UniXNU-sheng's SMEM, SMP2P, GLINK and GPR code (BSD-2-Clause-Patent, wsl/qcom/SOURCE.txt) goes in unchanged under
# bsd/dev/qcom/; pandora_adsp.c makes our SMP2P item before the DSP starts, waits for its ready bit, gives back the
# proxy votes at handover, brings up GLINK and asks the audio framework manager whether it is ready. The carveout and
# the register windows now come from ml_io_map_unmappable (the 85 MiB carveout is unmapped after the copy).
import os
os.makedirs('bsd/dev/qcom', exist_ok=True)
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
for f in ('QcomSmem.c', 'QcomSmem.h', 'QcomSmp2p.c', 'QcomSmp2p.h', 'QcomGlink.c', 'QcomGlink.h', 'ShengGpr.c',
          'ShengGpr.h', 'LICENSE.UniXNU', 'SOURCE.txt'):
    copy('qcom/' + f, 'bsd/dev/qcom/' + f)
edit('bsd/conf/files', '''bsd/dev/pandora_adsp.c\t\t\tstandard
''', '''bsd/dev/pandora_adsp.c\t\t\tstandard
bsd/dev/qcom/QcomSmem.c\t\t\tstandard
bsd/dev/qcom/QcomSmp2p.c\t\tstandard
bsd/dev/qcom/QcomGlink.c\t\tstandard
bsd/dev/qcom/ShengGpr.c\t\t\tstandard
''')
print('patched (ADSP SMP2P, GLINK, GPR)')
