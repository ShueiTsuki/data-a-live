import os
W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'
KIT = ['ArmSmmu4K', 'ArmSmmu4KContext', 'ArmSmmu4KProgram', 'ArmSmmu4KStreams', 'ArmSmmu4KMmio']


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# Adreno 840 bring-up, level 4a: the GMU's SMMU context (bsd/dev/pandora_gpu.c, pandora_gpu=4),
# with UniXNU's ArmSmmu4K kit (MIT, copied unchanged into bsd/dev/adreno/, see SOURCE.txt).
os.makedirs('bsd/dev/adreno', exist_ok=True)
for name in [f'{k}.{e}' for k in KIT for e in ('c', 'h')] + ['LICENSE.ArmSmmu4K', 'SOURCE.txt']:
    open('bsd/dev/adreno/' + name, 'w').write(open(W + 'adreno/' + name).read().replace('\r', ''))
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
patch('bsd/conf/files', [
    ("""bsd/dev/pandora_gpu.c			standard
""",
     """bsd/dev/pandora_gpu.c			standard
""" + ''.join(f'bsd/dev/adreno/{k}.c		standard\n' for k in KIT)),
])
print('patched')
