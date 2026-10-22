#!/usr/bin/env python3
"""PNG copy of the iPhone X default iOS 16 wallpaper (7400.WWDC_2022, 375w-812h@3x).

  mkwallpaper.py SRC_DIR OUT_DIR

SRC_DIR holds the three layer folders (*.ca) with index.xml, assetManifest.caml, main.caml
and the images already converted to PNG (heif-convert, libheif-plugin-libde265): HEIC
needs the hardware HEVC decoder, which is missing here, so PosterBoard crashes on them
(UniXNU f477f79 does the same for its iPadOS 17 wallpaper). OUT_DIR gets, per layer, the
PNG images and a main.caml whose <contents src="assets/X.HEIC"/> point at assets/X.png,
plus files.txt: "name mode path" lines for the kernel's file installer (patch 0147),
each name a flat file in OUT_DIR. Apple's files: keep OUT_DIR out of git (ref/).
"""
import shutil
import sys
from pathlib import Path

WALL = ('/Library/Wallpaper/Collections/iOS_16~iphone.wallpaperCollection/Wallpapers/'
        '7400.WWDC_2022-375w-812h@3x~iphone.wallpaper/')

src, out = Path(sys.argv[1]), Path(sys.argv[2])
if out.exists():
    shutil.rmtree(out)
out.mkdir(parents=True)
lines = []
for n, layer in enumerate(sorted(p for p in src.iterdir() if p.suffix == '.ca')):
    caml = (layer / 'main.caml').read_bytes().decode()
    for png in sorted(layer.glob('*.png')):
        stem = png.stem
        quoted = stem.replace(' ', '%20')
        old = f'src="assets/{quoted}.HEIC"'
        assert caml.count(old) == 1, (layer.name, old)
        caml = caml.replace(old, f'src="assets/{quoted}.png"')
        name = f'w{n}_{stem.replace(" ", "_")}.png'
        shutil.copy(png, out / name)
        lines.append(f'{name} 0644 {WALL}{layer.name}/assets/{stem}.png')
    assert 'HEIC"/>' not in caml, layer.name
    name = f'w{n}_main.caml'
    (out / name).write_bytes(caml.encode())
    lines.append(f'{name} 0644 {WALL}{layer.name}/main.caml')
(out / 'files.txt').write_bytes(('\n'.join(lines) + '\n').encode())
print('\n'.join(lines))
