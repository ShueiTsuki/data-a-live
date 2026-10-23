W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# Adreno 840 bring-up, level 11: the zap shader (PAS 13) and CP_SET_SECURE_MODE 0
# (bsd/dev/pandora_gpu.c, boot-arg pandora_gpu=11). pandora_adsp.c's PAS loader takes a full
# file name (a single-file .mbn such as gen80200_zap.mbn) and is exported for the GPU.
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
patch('bsd/dev/pandora_adsp.c', [
    ("""	char name[48];
	snprintf(name, sizeof(name), "%s.mdt", base);
	struct fw_ent *mdt = fw_find(name);""",
     """	char name[48];
	if (strchr(base, '.')) {
		strlcpy(name, base, sizeof(name));      /* a whole image, e.g. gen80200_zap.mbn */
	} else {
		snprintf(name, sizeof(name), "%s.mdt", base);
	}
	struct fw_ent *mdt = fw_find(name);"""),
    ("""static void
adsp_boot(void)
{""",
     """/* For bsd/dev/pandora_gpu.c: load FILE from the pinned pack as PAS image PAS into the
 * carveout, then authenticate and reset it. */
int pandora_pas_load_auth(const char *file, unsigned pas, uint64_t mem_pa, uint64_t mem_size);

int
pandora_pas_load_auth(const char *file, unsigned pas, uint64_t mem_pa, uint64_t mem_size)
{
	uint64_t bridge = 0;
	int err = ad_nfiles ? 0 : fw_dir();
	if (!err) {
		err = pas_load(file, pas, mem_pa, mem_size, &bridge);
	}
	if (!err) {
		err = pas_auth(pas);
	}
	return err;
}

static void
adsp_boot(void)
{"""),
])
print('patched')
