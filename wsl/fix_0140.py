p = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
s = open(p).read()


def sub(o, n):
    global s
    assert s.count(o) == 1, o[:80]
    s = s.replace(o, n)


# Factor the FIFO send out of sweet_dsi_brightness, add a DCS read-back of 0x52.
sub("""	level = level < SWEET_PANEL_BL_MIN ? SWEET_PANEL_BL_MIN :
	    (level > SWEET_PANEL_BL_MAX ? SWEET_PANEL_BL_MAX : level);
	/* clear CMD_MODE_DMA_DONE (write 1), keep the enables */
	sweet_dsi_wr(0x110, (sweet_dsi_rd(0x110) & 0xeeffffffu) | 1u);
	sweet_dsi_wr(0x15c, 0x30006u);
	/* header: word count 3, 0, DCS long write 0x39, last + long; payload 51 hi lo, pad ff */
	sweet_dsi_wr(0x17c, 0xc0390003u);
	sweet_dsi_wr(0x17c, 0xff000000u | ((level & 0xffu) << 16) | (((level >> 8) & 0xffu) << 8) | 0x51u);
	sweet_dsi_wr(0x3c, (sweet_dsi_rd(0x3c) & 0x2bffffffu) | 0x10000000u);
	sweet_dsi_wr(0x390, sweet_dsi_rd(0x390) & ~2u);
	sweet_dsi_wr(0x4c, 8);
	sweet_dsi_wr(0x90, 1);
	bool done = false;
	for (int i = 0; i < 50000 && !done; i++) {
		done = (sweet_dsi_rd(0x110) & 1u) != 0;
		if (!done) {
			IODelay(1);
		}
	}
	sweet_dsi_wr(0x15c, 0);
	sweet_dsi_wr(0x1ec, 1);
	IODelay(1);
	sweet_dsi_wr(0x1ec, 0);
	if (done) {
		sweet_dsi_wr(0x110, (sweet_dsi_rd(0x110) & 0xeeffffffu) | 1u);
	}
	if (sweet_dsi_logs++ < 20 || !done) {
		IOLog("SWEET DSI: brightness %u (%#x) %s\\n", level, level, done ? "sent" : "TIMEOUT");
	}
	if (!done) {
		sweet_dsi_bl = 0;       /* fall back to pixel scaling for the rest of the boot */
	}
	return done;
}""", """	level = level < SWEET_PANEL_BL_MIN ? SWEET_PANEL_BL_MIN :
	    (level > SWEET_PANEL_BL_MAX ? SWEET_PANEL_BL_MAX : level);
	/* header: word count 3, 0, DCS long write 0x39, last + long; payload 51 hi lo, pad ff */
	uint32_t cmd[2] = {0xc0390003u,
		           0xff000000u | ((level & 0xffu) << 16) | (((level >> 8) & 0xffu) << 8) | 0x51u};
	bool done = sweet_dsi_send(cmd, 2);
	bool log = sweet_dsi_logs++ < 20 || !done;
	if (log) {
		IOLog("SWEET DSI: brightness %u (%#x) %s\\n", level, level, done ? "sent" : "TIMEOUT");
	}
	if (!done) {
		sweet_dsi_bl = 0;       /* fall back to pixel scaling for the rest of the boot */
		return false;
	}
	if (log) {
		/* Read it back: set maximum return packet size 2 (0x37), then DCS read 0x52
		 * (short, last + bus turn-around); the answer lands in RDBK_DATA0..3. */
		uint32_t maxret = 0x80370002u, rd = 0xa0060052u;
		bool r1 = sweet_dsi_send(&maxret, 1);
		bool r2 = r1 && sweet_dsi_send(&rd, 1);
		IODelay(200);
		IOLog("SWEET DSI: read 0x52 %d/%d: count %#x ack_err %#x data %08x %08x %08x %08x type %#x\\n",
		    r1, r2, sweet_dsi_rd(0x1d4), sweet_dsi_rd(0x68), sweet_dsi_rd(0x6c), sweet_dsi_rd(0x70),
		    sweet_dsi_rd(0x74), sweet_dsi_rd(0x78), sweet_dsi_rd(0x7c));
	}
	return true;
}""")

sub("""static bool
sweet_dsi_brightness(uint32_t level)
{""", """/* One command (or several packed) through the test-pattern FIFO; true when DMA done. */
static bool
sweet_dsi_send(const uint32_t *w, unsigned n)
{
	/* clear CMD_MODE_DMA_DONE (write 1), keep the enables */
	sweet_dsi_wr(0x110, (sweet_dsi_rd(0x110) & 0xeeffffffu) | 1u);
	sweet_dsi_wr(0x15c, 0x30006u);
	for (unsigned i = 0; i < n; i++) {
		sweet_dsi_wr(0x17c, w[i]);
	}
	if (n & 1) {
		sweet_dsi_wr(0x17c, 0);         /* odd word count: one zero word more */
	}
	sweet_dsi_wr(0x3c, (sweet_dsi_rd(0x3c) & 0x2bffffffu) | 0x10000000u);
	sweet_dsi_wr(0x390, sweet_dsi_rd(0x390) & ~2u);
	sweet_dsi_wr(0x4c, 4 * n);
	sweet_dsi_wr(0x90, 1);
	bool done = false;
	for (int i = 0; i < 50000 && !done; i++) {
		done = (sweet_dsi_rd(0x110) & 1u) != 0;
		if (!done) {
			IODelay(1);
		}
	}
	sweet_dsi_wr(0x15c, 0);
	sweet_dsi_wr(0x1ec, 1);
	IODelay(1);
	sweet_dsi_wr(0x1ec, 0);
	if (done) {
		sweet_dsi_wr(0x110, (sweet_dsi_rd(0x110) & 0xeeffffffu) | 1u);
	}
	return done;
}

static bool
sweet_dsi_brightness(uint32_t level)
{""")
open(p, 'w').write(s)
print('patched')
