#!/bin/bash
# 0130: volume up at the real TLMM address (0x0f100000 + 0x1000 * 101), pandora_steps for the
# keys' step pauses, main screen touch driver bsd/dev/pandora_touch.c, SweetHIDTouch takes
# the presented display's pixels on PANDORA.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
W=/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl
tr -d '\r' < $W/pandora_touch.c > bsd/dev/pandora_touch.c
python3 - <<'PY'
def patch(p, subs):
    t = open(p).read()
    for old, new, what in subs:
        assert t.count(old) == 1, (p, what)
        t = t.replace(old, new)
    open(p, 'w').write(t)


patch('bsd/dev/fuxi_keys.c', [
    (""" * Volume up is gpio-keys on TLMM GPIO 101 (active low), TLMM 0x0f000000, 0x1000 per pin,
 * GPIO_IN_OUT at +4 bit 0 (pinctrl-kaanapali.c).""",
     """ * Volume up is gpio-keys on TLMM GPIO 101 (active low), 0x1000 per pin from 0x0f100000:
 * the DT says 0x0f000000 but the downstream pinctrl-canoe.ko adds 0x100000 (its pin table
 * holds 0x100000 + 0x1000 * n) and a read at 0x0f065000 hangs the bus.
 * GPIO_IN_OUT at +4 bit 0 (pinctrl-kaanapali.c).""", "comment"),
    ("#define PK_TLMM_GPIO   0x0f065000u   /* TLMM + 0x1000 * 101 */",
     "#define PK_TLMM_GPIO   0x0f165000u   /* 0x0f100000 + 0x1000 * 101 */", "addr"),
    ("""/* Before the first access to each register region: say which, then wait 2 s, so a reset
 * from a protected region leaves its name on the screen. */
static void
pk_step(const char *what, uint32_t pa)
{
	printf("PANDORA KEYS: next: first read of %s at %#x\\n", what, pa);
	fk_sleep(2000);
}""", """/* Boot-arg pandora_steps=MS: before the first access to each register region say which,
 * then wait MS, so a bus hang leaves its name in the log file. */
static unsigned pk_steps_ms;

static void
pk_step(const char *what, uint32_t pa)
{
	if (pk_steps_ms) {
		printf("PANDORA KEYS: next: first read of %s at %#x\\n", what, pa);
		fk_sleep(pk_steps_ms);
	}
}""", "step"),
    ("""	(void)PE_parse_boot_argn("pandora_keys", &pk_mask, sizeof(pk_mask));""",
     """	(void)PE_parse_boot_argn("pandora_keys", &pk_mask, sizeof(pk_mask));
	(void)PE_parse_boot_argn("pandora_steps", &pk_steps_ms, sizeof(pk_steps_ms));""", "arg"),
])

patch('bsd/dev/sweet_ufs.c', [
    ("extern void fuxi_keys_init(void);\n",
     "extern void fuxi_keys_init(void);\n#if defined(PANDORA)\nextern void pandora_touch_init(void);\n#endif\n", "decl"),
    ("""	/* Touch (bsd/dev/fuxi_touch.c; rails voted by the loader). */
	fuxi_touch_init();""", """	/* Touch (bsd/dev/fuxi_touch.c; rails voted by the loader). */
	fuxi_touch_init();
#if defined(PANDORA)
	/* Main screen touch, Synaptics TouchComm (bsd/dev/pandora_touch.c). */
	pandora_touch_init();
#endif""", "call"),
])

patch('bsd/conf/files', [
    ("bsd/dev/fuxi_touch.c\t\t\tstandard\n",
     "bsd/dev/fuxi_touch.c\t\t\tstandard\nbsd/dev/pandora_touch.c\t\t\tstandard\n", "files"),
])

patch('iokit/Kernel/arm/hid/SweetHID.cpp', [
    ("""	int fLogicalW, fLogicalH, fOffX, fOffY;
	bool fDown[SWEET_TOUCH_FINGERS];""", """	int fLogicalW, fLogicalH, fOffX, fOffY;
	int fPanelW, fPanelH;          /* coordinate range of touch() */
	bool fDown[SWEET_TOUCH_FINGERS];""", "member"),
    ("""	fOffX = (fLogicalW - 1080) / 2;
	fOffY = (fLogicalH - 2400) / 2;
""", """	fOffX = (fLogicalW - 1080) / 2;
	fOffY = (fLogicalH - 2400) / 2;
	fPanelW = 1080;
	fPanelH = 2400;
#if defined(PANDORA)
	/* bsd/dev/pandora_touch.c already reports in the presented display's pixels. */
	fPanelW = fLogicalW;
	fPanelH = fLogicalH;
	fOffX = fOffY = 0;
#endif
""", "start"),
    ("""	x = x < 0 ? 0 : (x > 1079 ? 1079 : x);
	y = y < 0 ? 0 : (y > 2399 ? 2399 : y);""", """	x = x < 0 ? 0 : (x > fPanelW - 1 ? fPanelW - 1 : x);
	y = y < 0 ? 0 : (y > fPanelH - 1 ? fPanelH - 1 : y);""", "clamp"),
    ("""		fEdge[finger] = x < SWEET_TOUCH_EDGE_PX || x > 1079 - SWEET_TOUCH_EDGE_PX ||
		    y < SWEET_TOUCH_EDGE_PX || y > 2399 - SWEET_TOUCH_EDGE_PX;""",
     """		fEdge[finger] = x < SWEET_TOUCH_EDGE_PX || x > fPanelW - 1 - SWEET_TOUCH_EDGE_PX ||
		    y < SWEET_TOUCH_EDGE_PX || y > fPanelH - 1 - SWEET_TOUCH_EDGE_PX;""", "edge"),
])
print('patched')
PY
git add -A
git diff --cached --stat
git diff --cached > $W/../xnu/patches/0130-pandora-volup-tlmm-and-touch.patch
git -c user.name=t -c user.email=t@t commit -qm 0130
