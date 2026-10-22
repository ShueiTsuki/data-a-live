p = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
s = open(p).read()


def sub(o, n):
    global s
    assert s.count(o) == 1, o[:80]
    s = s.replace(o, n)


sub("""extern "C" { int sweet_fb_display_off; }        /* read by bsd/dev/fuxi_touch.c */
""", """extern "C" { int sweet_fb_display_off; }        /* read by bsd/dev/fuxi_touch.c */
#if defined(PANDORA)
/*
 * Brightness from iOS (0136): kern_SwapSetBrightness puts a double and a "set" flag in
 * the SwapEnd record (flag 0x2e7, value 0x2ec; the limit: 0x2e8 / 0x2f4). The panel stays
 * at ABL's level; present() scales each pixel by sweet_fb_dim / 256 instead.
 */
static volatile uint32_t sweet_fb_dim = 256;
static uint32_t sweet_fb_bright_milli = ~0u, sweet_fb_limit_milli = ~0u;
static unsigned sweet_fb_bright_logs;

/* IEEE 754 double (little endian, maybe unaligned) -> value * 1000, clamped; no FP use. */
static uint32_t
sweet_fb_double_milli(const uint8_t *p)
{
	uint64_t b = 0;
	for (int i = 7; i >= 0; i--) {
		b = (b << 8) | p[i];
	}
	int e = (int)((b >> 52) & 0x7ff);
	if ((b >> 63) || e == 0) {
		return 0;
	}
	if (e == 0x7ff) {
		return 0xffffffffu;
	}
	uint64_t m = (b & ((1ULL << 52) - 1)) | (1ULL << 52);
	int sh = e - 1075;
	if (sh >= 0) {
		return 0xffffffffu;
	}
	if (sh <= -64) {
		return 0;
	}
	uint64_t v = (m * 1000) >> -sh;     /* m < 2^53: m * 1000 < 2^63 */
	return v > 0xffffffffULL ? 0xffffffffu : (uint32_t)v;
}

/* Brightness value -> scale: 0..1 is a fraction; above 1 it is taken as nits of a
 * 500-nit panel. Never below 1/16 so the screen stays readable. */
static void
sweet_fb_brightness(const uint8_t *rec)
{
	uint32_t b = rec[0x2e7] ? sweet_fb_double_milli(rec + 0x2ec) : sweet_fb_bright_milli;
	uint32_t l = rec[0x2e8] ? sweet_fb_double_milli(rec + 0x2f4) : sweet_fb_limit_milli;
	if (b == sweet_fb_bright_milli && l == sweet_fb_limit_milli) {
		return;
	}
	sweet_fb_bright_milli = b;
	sweet_fb_limit_milli = l;
	if (b == ~0u) {
		return;
	}
	uint64_t frac = b <= 1000 ? b : (uint64_t)b / 500;      /* 0..1000 */
	if (l != ~0u && l != 0) {
		uint64_t lf = l <= 1000 ? l : (uint64_t)l / 500;
		frac = frac < lf ? frac : lf;
	}
	frac = frac > 1000 ? 1000 : frac;
	uint32_t dim = (uint32_t)(frac * 256 / 1000);
	sweet_fb_dim = dim < 16 ? 16 : dim;
	if (sweet_fb_bright_logs++ < 40) {
		IOLog("SWEET FB: brightness %u.%03u limit %u.%03u -> scale %u/256\\n", b / 1000, b % 1000,
		    l == ~0u ? 0 : l / 1000, l == ~0u ? 0 : l % 1000, sweet_fb_dim);
	}
}

static inline uint32_t
sweet_fb_scale(uint32_t p, uint32_t k)
{
	uint32_t rb = ((p & 0x00ff00ffu) * k >> 8) & 0x00ff00ffu;
	uint32_t g = (((p >> 8) & 0x000000ffu) * k) & 0x0000ff00u;
	return (p & 0xff000000u) | rb | g;
}
#endif
""")

sub("""				for (uint32_t Y = py0; Y < py1; Y++) {
					uint32_t ly = (uint32_t)((uint64_t)Y * fLogicalH / fHeight);
					const uint32_t *srow = (const uint32_t *)(src + s->offset +
					    (uint64_t)(sy + (int32_t)ly - dy) * s->bytesPerRow);
					uint32_t *drow = (uint32_t *)(fPixels + (uint64_t)Y * fRowBytes);
					for (uint32_t X = px0; X < px1; X++) {
						drow[X] = srow[sx + (int32_t)fFillX[X] - dx];
					}
				}""", """				uint32_t k = sweet_fb_dim;
				for (uint32_t Y = py0; Y < py1; Y++) {
					uint32_t ly = (uint32_t)((uint64_t)Y * fLogicalH / fHeight);
					const uint32_t *srow = (const uint32_t *)(src + s->offset +
					    (uint64_t)(sy + (int32_t)ly - dy) * s->bytesPerRow);
					uint32_t *drow = (uint32_t *)(fPixels + (uint64_t)Y * fRowBytes);
					if (k >= 256) {
						for (uint32_t X = px0; X < px1; X++) {
							drow[X] = srow[sx + (int32_t)fFillX[X] - dx];
						}
					} else {
						for (uint32_t X = px0; X < px1; X++) {
							drow[X] = sweet_fb_scale(srow[sx + (int32_t)fFillX[X] - dx], k);
						}
					}
				}""")

sub("""		memcpy(rec, args->structureInput, sizeof(rec));
		fFB->swap(rec);
		break;
	}""", """		memcpy(rec, args->structureInput, sizeof(rec));
#if defined(PANDORA)
		sweet_fb_brightness(rec);
#endif
		fFB->swap(rec);
		break;
	}
#if defined(PANDORA)
	case 50:        /* IOMobileFramebufferSetBrightnessCorrection(fb, value): logged only */
		if (args->scalarInputCount >= 2 && sweet_fb_bright_logs++ < 40) {
			IOLog("SWEET FB: brightness correction %#llx %#llx\\n", args->scalarInput[0],
			    args->scalarInput[1]);
		}
		break;
#endif""")
open(p, 'w').write(s)
print('patched')
