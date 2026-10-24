W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# Boot like an iPhone: iBoot draws the Apple logo and, without -v, xnu shows no text over it; the
# logo stays until the system presents its first frame. Here shim/loader (v2) draws the logo, and
# the framebuffer "serial" console (ios-sweet's fbcon, which drew every kprintf line from the start
# and cleared the screen first) starts muted unless the boot-args have -v. The text still reaches
# the persistent log, a panic still draws (serial_console.c), and the first frame that shows a
# surface clears the panel as before (SweetFramebuffer.cpp, 0156/0157).
S = 'pexpert/arm/pe_serial.c'
edit(S, '''static void
fbcon_init(void)
{
	fbcon_cols = fbcon_width / (8 * FBCON_SCALE);
	fbcon_rows = fbcon_height / (16 * FBCON_SCALE);
	fbcon_col = fbcon_row = 0;
''', '''#if defined(PANDORA)
extern int sweet_console_muted;         /* osfmk/console/serial_console.c */
#endif

static void
fbcon_init(void)
{
	fbcon_cols = fbcon_width / (8 * FBCON_SCALE);
	fbcon_rows = fbcon_height / (16 * FBCON_SCALE);
	fbcon_col = fbcon_row = 0;
#if defined(PANDORA)
	/* 0206: without -v the loader's boot logo stays; the text only goes to the log. */
	int verbose = 0;
	if (!PE_parse_boot_argn("-v", &verbose, sizeof(verbose))) {
		sweet_console_muted = 1;
		return;
	}
#endif
''')
print('patched')
