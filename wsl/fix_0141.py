p = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
s = open(p).read()


def sub(o, n):
    global s
    assert s.count(o) == 1, o[:80]
    s = s.replace(o, n)


sub("""		uint32_t maxret = 0x80370002u, rd = 0xa0060052u;
		bool r1 = sweet_dsi_send(&maxret, 1);
		bool r2 = r1 && sweet_dsi_send(&rd, 1);
		IODelay(200);
		IOLog("SWEET DSI: read 0x52 %d/%d: count %#x ack_err %#x data %08x %08x %08x %08x type %#x\\n",
		    r1, r2, sweet_dsi_rd(0x1d4), sweet_dsi_rd(0x68), sweet_dsi_rd(0x6c), sweet_dsi_rd(0x70),
		    sweet_dsi_rd(0x74), sweet_dsi_rd(0x78), sweet_dsi_rd(0x7c));""", """		/* RDBK_DATA0 then holds 22 hi lo ecc (DCS short read response, 2 bytes). Read
		 * right away and again 100 ms later (frames from the MDP in between): the first
		 * test read the old level 0x394 200 us after the write. */
		uint32_t maxret = 0x80370002u, rd = 0xa0060052u;
		for (int pass = 0; pass < 2; pass++) {
			if (pass) {
				IOSleep(100);
			}
			bool r1 = sweet_dsi_send(&maxret, 1);
			bool r2 = r1 && sweet_dsi_send(&rd, 1);
			IODelay(200);
			uint32_t d0 = sweet_dsi_rd(0x6c);
			IOLog("SWEET DSI: read 0x52 (%s) %d/%d: count %#x ack_err %#x data %08x %08x -> level %u\\n",
			    pass ? "+100 ms" : "now", r1, r2, sweet_dsi_rd(0x1d4), sweet_dsi_rd(0x68), d0,
			    sweet_dsi_rd(0x70), (d0 >> 8) & 0xffffu);
		}""")
open(p, 'w').write(s)
print('patched')
