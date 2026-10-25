W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60], s.count(old))
    open(path, 'w').write(s.replace(old, new))


# ---- 0229: the ADSP images from the device's own firmware partition ---------------------------------------------
# As Android and UniXNU-sheng: /image of modem_<slot> (Android's /vendor/firmware_mnt, LUN 4), a FAT volume read in
# place, read only. sweet_ufs.c finds one more GPT partition by name on any LUN and reads bytes of it
# (pandora_part_find, pandora_part_pread); UniXNU-sheng's FatVolume.c (unchanged) reads the volume; pandora_adsp.c
# takes the ADSP images from it (boot-arg pandora_slot=a|b, from tools/boot_ios.sh) and the GPU's from the pack as
# before. UniXNU-sheng's newer QcomGlink keeps a larger remote intent when the table is full (48 entries now).
copy('pandora_adsp.c', 'bsd/dev/pandora_adsp.c')
for f in ('QcomGlink.c', 'QcomGlink.h', 'FatVolume.c', 'FatVolume.h', 'SOURCE.txt'):
    copy('qcom/' + f, 'bsd/dev/qcom/' + f)
edit('bsd/conf/files', 'bsd/dev/qcom/ShengGpr.c\t\t\tstandard\n',
     'bsd/dev/qcom/ShengGpr.c\t\t\tstandard\nbsd/dev/qcom/FatVolume.c\t\tstandard\n')

U = 'bsd/dev/sweet_ufs.c'
# The GPT lookup without the root partition's state, for both callers.
edit(U, 'static int\nufs_find_partition(const char *want)\n{\n\tconst uint8_t *b',
     '/* The GPT partition named want on the LUN su.lun, in device blocks (0229: pandora_part_find too). */\n'
     'static int\nufs_gpt_lookup(const char *want, uint64_t *first_out, uint64_t *nblocks_out)\n{\n\tconst uint8_t *b')
edit(U, '\t\tsu.first_block = first;\n\t\tsu.nblocks = last - first + 1;\n\t\treturn 0;\n\t}\n'
        '\tprintf("SWEET UFS: no GPT partition named \\"%s\\"\\n", want);\n\treturn ENOENT;\n}\n',
     '\t\t*first_out = first;\n\t\t*nblocks_out = last - first + 1;\n\t\treturn 0;\n\t}\n\treturn ENOENT;\n}\n'
     '\nstatic int\nufs_find_partition(const char *want)\n{\n'
     '\tint error = ufs_gpt_lookup(want, &su.first_block, &su.nblocks);\n'
     '\tif (error == ENOENT) {\n\t\tprintf("SWEET UFS: no GPT partition named \\"%s\\"\\n", want);\n\t}\n'
     '\treturn error;\n}\n')
edit(U, '\tlck_mtx_unlock(&sweet_ufs_lock);\n\treturn error;\n}\n#endif /* PANDORA */\n#endif /* ARM64_BOARD_CONFIG_SWEET */',
     '''\tlck_mtx_unlock(&sweet_ufs_lock);
\treturn error;
}

/*
 * 0229: one more GPT partition, read only, on any LUN, for bsd/dev/pandora_adsp.c: the firmware
 * partition modem_<slot> (LUN 4; Android's /vendor/firmware_mnt), as UniXNU-sheng's
 * native_ufs_part_find and native_ufs_part_read. Each call switches su.lun for its commands under
 * sweet_ufs_lock and sets it back to 0; ufs_scsi's filter passes only TEST UNIT READY and READ(10)
 * there, and pandora_part_pread stays inside the partition found. LUN 4 is asked first, so on
 * pandora no other LUN is touched.
 */
static struct {
\tuint8_t lun;
\tuint64_t first, blocks;
} pp;

int pandora_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
int
pandora_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)
{
\tstatic const uint8_t tur[6] = { 0x00 };
\tstatic const uint8_t order[8] = { 4, 0, 1, 2, 3, 5, 6, 7 };
\tint error = ENOENT;
\tif (!su.ready) {
\t\treturn ENXIO;
\t}
\tlck_mtx_lock(&sweet_ufs_lock);
\tpp.blocks = 0;
\tfor (unsigned i = 0; i < 8 && error; i++) {
\t\tint e = 0;
\t\tuint64_t f = 0, n = 0;
\t\tsu.lun = order[i];
\t\t/* The first command to a LUN may report a unit attention. */
\t\tfor (int t = 0; t < 3 && (e = ufs_scsi(tur, sizeof(tur), 0, false)); t++) {
\t\t}
\t\tif (!e && !ufs_gpt_lookup(name, &f, &n)) {
\t\t\tpp.lun = order[i];
\t\t\tpp.first = f;
\t\t\tpp.blocks = n;
\t\t\terror = 0;
\t\t}
\t}
\tsu.lun = 0;
\tlck_mtx_unlock(&sweet_ufs_lock);
\tif (!error) {
\t\t*lun = pp.lun;
\t\t*first = pp.first;
\t\t*blocks = pp.blocks;
\t}
\treturn error;
}

/* Copy len bytes at byte offset off of that partition into dst. */
int pandora_part_pread(uint64_t off, void *dst, uint64_t len);
int
pandora_part_pread(uint64_t off, void *dst, uint64_t len)
{
\tif (!pp.blocks || off + len < off || off + len > pp.blocks * UFS_BLOCK) {
\t\treturn EINVAL;
\t}
\tuint8_t *out = dst;
\tint error = 0;
\tlck_mtx_lock(&sweet_ufs_lock);
\tsu.lun = pp.lun;
\twhile (len && !error) {
\t\tuint64_t blk = off / UFS_BLOCK;
\t\tuint32_t in = (uint32_t)(off % UFS_BLOCK);
\t\tuint64_t need = (in + len + UFS_BLOCK - 1) / UFS_BLOCK;
\t\tuint32_t n = (uint32_t)MIN(need, (uint64_t)(BOUNCE_SIZE / UFS_BLOCK));
\t\terror = ufs_read_blocks(pp.first + blk, n);
\t\tif (error) {
\t\t\tbreak;
\t\t}
\t\tuint64_t got = MIN((uint64_t)n * UFS_BLOCK - in, len);
\t\tmemcpy(out, (const uint8_t *)(su.dma + DMA_BOUNCE) + in, (size_t)got);
\t\tout += got;
\t\toff += got;
\t\tlen -= got;
\t}
\tsu.lun = 0;
\tlck_mtx_unlock(&sweet_ufs_lock);
\treturn error;
}
#endif /* PANDORA */
#endif /* ARM64_BOARD_CONFIG_SWEET */''')
print('patched (ADSP firmware from the firmware partition)')
