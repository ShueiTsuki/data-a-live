W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# IOSurface: a row size or plane size of 0 in the creation properties means "the kernel picks", as Apple's
# IOSurface reads it. QuartzCore creates its full-screen two-plane extended-range surfaces ('b3a8') with
# IOSurfacePlaneBytesPerRow 0 and its own offsets and allocation size; SweetSurface kept the 0, so
# IOSurfaceGetBytesPerRowOfPlane answered 0 and the Metal plugin refused the layer (bb30-bb44: the App
# Library background). Rows are now computed (64-byte aligned) when 0 is given; the first 'b3a8'
# surfaces log what was asked and what was made.
S = 'iokit/Kernel/arm/SweetSurface.cpp'
edit(S, '''sweet_dict_u32(OSDictionary *dict, const char *key, uint32_t def)
{
	OSNumber *n = OSDynamicCast(OSNumber, dict->getObject(key));
	return n != NULL ? n->unsigned32BitValue() : def;
}
''', '''sweet_dict_u32(OSDictionary *dict, const char *key, uint32_t def)
{
	OSNumber *n = OSDynamicCast(OSNumber, dict->getObject(key));
	return n != NULL ? n->unsigned32BitValue() : def;
}

/* 0199: a size where 0 means "the kernel picks" (Apple's IOSurface reads it so). */
static uint32_t
sweet_dict_u32_nz(OSDictionary *dict, const char *key, uint32_t def)
{
	uint32_t v = sweet_dict_u32(dict, key, 0);
	return v != 0 ? v : def;
}
''')
edit(S, 's->bytesPerRow = sweet_dict_u32(props, "IOSurfaceBytesPerRow",',
     's->bytesPerRow = sweet_dict_u32_nz(props, "IOSurfaceBytesPerRow",')
edit(S, 'p->bytesPerRow = sweet_dict_u32(pd, "IOSurfacePlaneBytesPerRow",',
     'p->bytesPerRow = sweet_dict_u32_nz(pd, "IOSurfacePlaneBytesPerRow",')
edit(S, 'p->size = sweet_dict_u32(pd, "IOSurfacePlaneSize",',
     'p->size = sweet_dict_u32_nz(pd, "IOSurfacePlaneSize",')
edit(S, '''		s->planeCount = planes->getCount();
''', '''		s->planeCount = planes->getCount();
		/* 0199: what QuartzCore asks of its 'b3a8' surfaces and what they get, the first few */
		static uint32_t b3a8_told;
		if (s->pixelFormat == 0x62336138 && b3a8_told < 8) {
			b3a8_told++;
			for (unsigned int i = 0; i < planes->getCount(); i++) {
				OSDictionary *pd = OSDynamicCast(OSDictionary, planes->getObject(i));
				IOLog("SWEET SURF: b3a8 %ux%u plane %u asked row %d size %d offset %d bpe %d -> row %u size %u "
				    "offset %u (alloc asked %d)\\n", s->width, s->height, i,
				    (int)sweet_dict_u32(pd, "IOSurfacePlaneBytesPerRow", (uint32_t)-1),
				    (int)sweet_dict_u32(pd, "IOSurfacePlaneSize", (uint32_t)-1),
				    (int)sweet_dict_u32(pd, "IOSurfacePlaneOffset", (uint32_t)-1),
				    (int)sweet_dict_u32(pd, "IOSurfacePlaneBytesPerElement", (uint32_t)-1),
				    s->planes[i].bytesPerRow, s->planes[i].size, s->planes[i].offset,
				    (int)sweet_dict_u32(props, "IOSurfaceAllocSize", (uint32_t)-1));
			}
		}
''')

# GPU user client: short fence polls and long waits that timed out, counted apart.
copy('PandoraAdrenoGpu.cpp', 'iokit/Kernel/arm/PandoraAdrenoGpu.cpp')
print('patched')
