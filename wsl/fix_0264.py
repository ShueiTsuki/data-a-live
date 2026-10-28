def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0264: the panel's own dimming off by default -----------------------------------------------------------------
# bl1 (k0263): the user found brightness changes too slow. Dragging the slider, CoreBrightness sent a new
# "brightness" every 8 ms with FadePeriod 0 and each went to the panel at once; the panel's dimming (53 28) then
# took each change over slowly. iOS already sends the steps finely, so the panel's dimming stays off as ABL left it
# (boot-arg pandora_bl_dim=1 turns it on). FadePeriod (dimming and waking the screen) is still followed by the thread.
F = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
edit(F, '''		if (sweet_dsi_bl == 1) {
			sweet_bl_boot = sweet_dsi_read_level();
			/* the panel's dimming on: DCS short write 0x15, 53 28 (DT mi,mdss-dsi-dimmingon-command) */
			uint32_t dim = 0x80152853u;
			bool ok = sweet_dsi_send(&dim, 1);
			IOLog("SWEET DSI: panel level at start %u (%#x), panel dimming on %s\\n", sweet_bl_boot, sweet_bl_boot,
			    ok ? "sent" : "TIMEOUT");
		}''', '''		if (sweet_dsi_bl == 1) {
			sweet_bl_boot = sweet_dsi_read_level();
			/* 0264: the panel's dimming only with pandora_bl_dim=1 (DCS short write 0x15, 53 28, DT
			 * mi,mdss-dsi-dimmingon-command): it made each slider step slow (bl1). */
			int dim_on = 0;
			PE_parse_boot_argn("pandora_bl_dim", &dim_on, sizeof(dim_on));
			bool ok = true;
			if (dim_on) {
				uint32_t dim = 0x80152853u;
				ok = sweet_dsi_send(&dim, 1);
			}
			IOLog("SWEET DSI: panel level at start %u (%#x), panel dimming %s\\n", sweet_bl_boot, sweet_bl_boot,
			    !dim_on ? "off" : ok ? "on" : "TIMEOUT");
		}''')
print('patched (0264 panel dimming off)')
