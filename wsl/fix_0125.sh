#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
t = open(p).read()


def sub(old, new, what):
    global t
    assert t.count(old) == 1, what
    t = t.replace(old, new)


sub("""	if (!native && fWidth <= SWEET_FB_LOGICAL_W && fHeight <= SWEET_FB_LOGICAL_H) {""",
    """#if defined(PANDORA)
	/*
	 * The 17 Pro panel (1220x2656) is larger than the iPhone X one. Presented at its
	 * own size, backboardd/UIKit have no layout for it and backboardd loops looking
	 * for AppleM2ScalerCSCDriver. Present 1125x2436 and center it (47 px left/right,
	 * 110 px top/bottom black border); the offsets below are then negative.
	 */
	if (!native) {
#else
	if (!native && fWidth <= SWEET_FB_LOGICAL_W && fHeight <= SWEET_FB_LOGICAL_H) {
#endif""", "logical")

sub("""			sweet_console_muted = 1;
		}
	}
""", """			sweet_console_muted = 1;
#if defined(PANDORA)
			/* The border around the centered iPhone X area keeps the last console
			 * text otherwise: clear the whole panel once. */
			for (uint32_t y = 0; y < fHeight; y++) {
				bzero((void *)(fPixels + (uint64_t)y * fRowBytes), (size_t)fWidth * 4);
			}
#endif
		}
	}
""", "clear")
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0125-pandora-present-iphone-x-centered.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0125
