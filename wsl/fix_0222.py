W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- 0222: a sampling profiler for one process (off unless boot-arg pandora_prof=NAME) -------------------------------
# The translation layer's costs on the phone differ from lavapipe's on the PC (Turnip suballocates descriptor sets,
# lavapipe maps memory for each; iOS's malloc is not glibc's), so its optimisation is measured where it runs: about
# 1000 samples a second of where the named process's threads are (user PC; or kernel PC and the user PC it entered
# from), the hottest PCs logged every 10 s with the images dyld loaded (tools/prof.py names them).
copy('pandora_prof.c', 'osfmk/arm/pandora_prof.c')
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
edit('osfmk/conf/files.arm64', '''osfmk/arm/pandora_perfctl.c	standard
''', '''osfmk/arm/pandora_perfctl.c	standard
osfmk/arm/pandora_prof.c	standard
''')
print('patched (profiler)')
