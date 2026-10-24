W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# IOSurface: a plane's row size goes at +0x10 of its lock-result entry, not +0x08. IOSurface.framework (iOS 16)
# copies the lock result to IOSurfaceClient + 0x68 and IOSurfaceClientGetBytesPerRowOfPlane reads client
# + 0xe0 + plane * 0x80, which is result + 0x68 + plane * 0x80 + 0x10 (width +0x00, height +0x04, offset +0x0c,
# size +0x14, bytes per element +0x18, element width/height +0x1a/+0x1b all match already; nothing reads
# +0x08). So every planar surface answered 0 for its plane rows: QuartzCore's 'b3a8' layers (the App Library
# background, bb30-bb45, covered since by the Metal plugin computing the planes) and any video ('420v') surface.
S = 'iokit/Kernel/arm/SweetSurface.cpp'
edit(S, '''		put32(p, 0x08, pl->bytesPerRow);
''', '''		put32(p, 0x10, pl->bytesPerRow);   /* 0204: IOSurfaceClientGetBytesPerRowOfPlane reads +0x10 */
''')

# CPU governor: no cap at the limits read back (255 / 0 since the fast channels), the firmware's level taken.
copy('pandora_cpu.c', 'bsd/dev/pandora_cpu.c')
print('patched')
