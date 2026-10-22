#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/kern/ubc_subr.c'
t = open(p).read()


def sub(old, new, what):
    global t
    assert t.count(old) == 1, what
    t = t.replace(old, new)


# Read the signed range in 1 MiB pieces (64 original 16K blocks per vn_rdwr) instead of
# one 16 KiB read per block: the dyld shared cache alone is ~3.3 GB, i.e. ~200k
# synchronous reads on the polled UFS driver. Every 16K block is still hashed and
# compared with the code directory before any 4K digest derived from it is stored.
sub("""	uint8_t *buffer = NULL;
	uint64_t limit, offset;
	int error = EBADEXEC;
	kern_return_t kr;

	if (blob->csb_hash_pageshift <= PAGE_SHIFT) {""",
"""	uint8_t *buffer = NULL;
	vm_size_t bufsize = 1024 * 1024;        /* a multiple of 16384 */
	uint64_t limit, chunk;
	int error = EBADEXEC;
	kern_return_t kr;

	if (blob->csb_hash_pageshift <= PAGE_SHIFT) {""", "decl")

old_loop_start = t.index("	buffer = kalloc_data(16384, Z_WAITOK);\n	for (offset = 0; offset < limit; offset += 16384) {")
old_loop_end = t.index("	kr = vm_map_protect(kernel_map, storage, storage + allocation, VM_PROT_READ, TRUE);", old_loop_start)
t = t[:old_loop_start] + """	buffer = kalloc_data(bufsize, Z_WAITOK);
	if (buffer == NULL) {
		bufsize = 16384;
		buffer = kalloc_data(bufsize, Z_WAITOK);
	}
	if (buffer == NULL) {
		error = ENOMEM;
		goto out;
	}
	for (chunk = 0; chunk < limit; chunk += bufsize) {
		size_t chunk_length = (size_t)MIN((uint64_t)bufsize, limit - chunk);
		int resid = 0;
		off_t position;
		if (os_add_overflow(blob->csb_base_offset, (off_t)chunk, &position)) {
			error = EOVERFLOW;
			goto out;
		}
		error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, (int)chunk_length, position,
		    UIO_SYSSPACE, 0, vfs_context_ucred(vfs_context_current()),
		    &resid, current_proc());
		if (error || resid) {
			error = error ? error : EIO;
			goto out;
		}
		for (size_t in = 0; in < chunk_length; in += 16384) {
			union cs_hash_union ctx;
			uint8_t digest[CS_HASH_MAX_SIZE];
			const uint8_t *expected;
			const char *lower = blob->csb_mem_kaddr;
			const char *upper = lower + blob->csb_mem_size;
			uint64_t offset = chunk + in;
			size_t length = MIN((size_t)16384, chunk_length - in);
			const uint8_t *block = buffer + in;
			expected = hashes(cd, (uint32_t)(offset >> 14), md->cs_size, lower, upper);
			if (expected == NULL) {
				error = EBADEXEC;
				goto out;
			}
			md->cs_init(&ctx);
			md->cs_update(&ctx, block, length);
			md->cs_final(digest, &ctx);
			if (bcmp(expected, digest, md->cs_size)) {
				os_atomic_inc(&cs_validate_page_bad_hash, relaxed);
				error = EBADEXEC;
				goto out;
			}
			os_atomic_inc(&cs_native4k_full_blocks, relaxed);
			for (size_t sub = 0; sub < length; sub += PAGE_SIZE) {
				size_t bytes = MIN((size_t)PAGE_SIZE, length - sub);
				vm_size_t index = (offset + sub) >> PAGE_SHIFT;
				md->cs_init(&ctx);
				md->cs_update(&ctx, block + sub, bytes);
				md->cs_final(digest, &ctx);
				memcpy((void *)(storage + index * md->cs_size), digest, md->cs_size);
			}
		}
	}
""" + t[old_loop_end:]

sub("""out:
	kfree_data(buffer, 16384);
	if (storage) {
		kmem_free(kernel_map, storage, allocation);
	}
	return error;
}""", """out:
	if (buffer != NULL) {
		kfree_data(buffer, bufsize);
	}
	if (storage) {
		kmem_free(kernel_map, storage, allocation);
	}
	return error;
}""", "out")
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0127-pandora-cs-native4k-1mib-reads.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0127
