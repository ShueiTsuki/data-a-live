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


sub("""	int32_t       fOffX, fOffY;             /* logical origin of the panel */
""", """	int32_t       fOffX, fOffY;             /* logical origin of the panel */
#if defined(PANDORA)
	bool          fFill;                    /* sweetfb_fill=1: scale to the whole panel */
	uint16_t      fFillX[2048];             /* panel x -> logical x */
#endif
""", "members")

sub("""	fOffX = (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = (int32_t)(fLogicalH - fHeight) / 2;
""", """	fOffX = (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = (int32_t)(fLogicalH - fHeight) / 2;
#if defined(PANDORA)
	/*
	 * sweetfb_fill=1: instead of the centered 1125x2436 area with a black border,
	 * scale the presented display to the whole 1220x2656 panel (nearest neighbour,
	 * x 1.084 / y 1.090; the aspect ratios differ by 0.5 %).
	 */
	int fill = 0;
	PE_parse_boot_argn("sweetfb_fill", &fill, sizeof(fill));
	fFill = fill != 0 && fLogicalW != fWidth && fWidth <= 2048;
	for (uint32_t x = 0; fFill && x < fWidth; x++) {
		fFillX[x] = (uint16_t)((uint64_t)x * fLogicalW / fWidth);
	}
#endif
""", "start")

sub("""		/* Logical (presented) display coordinates -> panel coordinates. */
		dx -= fOffX;
		dy -= fOffY;
""", """#if defined(PANDORA)
		if (fFill) {
			/* Clip in logical space, check the source, then for every panel pixel
			 * whose logical position falls in the layer's rectangle copy that pixel. */
			if (w > (int32_t)s->width - sx) {
				w = (int32_t)s->width - sx;
			}
			if (h > (int32_t)s->height - sy) {
				h = (int32_t)s->height - sy;
			}
			if (w > (int32_t)fLogicalW - dx) {
				w = (int32_t)fLogicalW - dx;
			}
			if (h > (int32_t)fLogicalH - dy) {
				h = (int32_t)fLogicalH - dy;
			}
			uint64_t lastf = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +
			    (uint64_t)(sx + w) * 4;
			if (src != 0 && bpe == 4 && w > 0 && h > 0 && s->elementWidth == 1 &&
			    s->elementHeight == 1 && s->memory != NULL && lastf <= s->memory->getLength()) {
				uint32_t px0 = (uint32_t)(((uint64_t)dx * fWidth + fLogicalW - 1) / fLogicalW);
				uint32_t px1 = (uint32_t)MIN(((uint64_t)(dx + w) * fWidth + fLogicalW - 1) / fLogicalW,
				    (uint64_t)fWidth);
				uint32_t py0 = (uint32_t)(((uint64_t)dy * fHeight + fLogicalH - 1) / fLogicalH);
				uint32_t py1 = (uint32_t)MIN(((uint64_t)(dy + h) * fHeight + fLogicalH - 1) / fLogicalH,
				    (uint64_t)fHeight);
				for (uint32_t Y = py0; Y < py1; Y++) {
					uint32_t ly = (uint32_t)((uint64_t)Y * fLogicalH / fHeight);
					const uint32_t *srow = (const uint32_t *)(src + s->offset +
					    (uint64_t)(sy + (int32_t)ly - dy) * s->bytesPerRow);
					uint32_t *drow = (uint32_t *)(fPixels + (uint64_t)Y * fRowBytes);
					for (uint32_t X = px0; X < px1; X++) {
						drow[X] = srow[sx + (int32_t)fFillX[X] - dx];
					}
				}
				cpuFrame = true;
			}
			s->release();
			continue;
		}
#endif
		/* Logical (presented) display coordinates -> panel coordinates. */
		dx -= fOffX;
		dy -= fOffY;
""", "present")
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0126-pandora-sweetfb-fill-scaling.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0126
