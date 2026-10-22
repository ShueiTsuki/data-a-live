p = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
s = open(p).read()


def sub(o, n):
    global s
    assert s.count(o) == 1, o[:80]
    s = s.replace(o, n)


sub("""OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);
""", """OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

#if defined(PANDORA)
/*
 * Screen thumbnail into the log (0142), for bsd/dev/pandora_selftest.c: 60 x 130 samples
 * of the panel memory (what is really shown), one hex digit of grey each, one line per
 * row, "SWEET THUMB <tag> <row> <60 digits>". tools/thumb.py turns it into a PNG.
 */
static AppleCLCD *gSweetCLCD;
extern "C" void sweet_fb_thumb(const char *tag);
extern "C" void
sweet_fb_thumb(const char *tag)
{
	AppleCLCD *fb = gSweetCLCD;
	if (!fb || !fb->fPixels) {
		return;
	}
	const uint32_t cols = 60, rows = 130;
	char line[64];
	for (uint32_t r = 0; r < rows; r++) {
		uint32_t y = (uint32_t)(((uint64_t)r * 2 + 1) * fb->fHeight / (2 * rows));
		const volatile uint32_t *row = (const volatile uint32_t *)(fb->fPixels + (uint64_t)y * fb->fRowBytes);
		for (uint32_t c = 0; c < cols; c++) {
			uint32_t x = (uint32_t)(((uint64_t)c * 2 + 1) * fb->fWidth / (2 * cols));
			uint32_t px = row[x];
			uint32_t g = (((px >> 16) & 0xff) * 77 + ((px >> 8) & 0xff) * 150 + (px & 0xff) * 29) >> 12;
			line[c] = "0123456789abcdef"[g & 15];
		}
		line[cols] = 0;
		IOLog("SWEET THUMB %s %03u %s\\n", tag, r, line);
	}
}
#endif
""")

sub("""	registerService();
#if defined(PANDORA)
	{
		AppleARMBacklight *bl""", """	registerService();
#if defined(PANDORA)
	gSweetCLCD = this;
	{
		AppleARMBacklight *bl""")
open(p, 'w').write(s)

# The self-test thread.
open('bsd/dev/pandora_selftest.c', 'w').write(open('/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/pandora_selftest.c').read().replace('\r', ''))

p = 'bsd/dev/sweet_ufs.c'
s = open(p).read()
sub("extern void pandora_amp_init(void);\n", "extern void pandora_amp_init(void);\nextern void pandora_selftest_init(void);\n")
sub("""	pandora_amp_init();
#endif""", """	pandora_amp_init();
	/* Input self-test, boot-arg pandora_selftest (bsd/dev/pandora_selftest.c). */
	pandora_selftest_init();
#endif""")
open(p, 'w').write(s)

p = 'bsd/conf/files'
s = open(p).read()
sub("bsd/dev/pandora_amp.c\t\t\tstandard\n", "bsd/dev/pandora_amp.c\t\t\tstandard\nbsd/dev/pandora_selftest.c\t\tstandard\n")
open(p, 'w').write(s)
print('patched')
