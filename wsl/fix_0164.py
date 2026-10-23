W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# Adreno 840 bring-up, level 3: GPLL0 votes for the GPU and SCM GPU_INIT_REGS
# (bsd/dev/pandora_gpu.c, boot-arg pandora_gpu=3). pandora_adsp.c's SCM call is shared.
open('bsd/dev/pandora_gpu.c', 'w').write(open(W + 'pandora_gpu.c').read().replace('\r', ''))
patch('bsd/dev/pandora_adsp.c', [
    ("""#define SCM_FN(svc, cmd)  ((1ULL << 30) | (2ULL << 24) | (((uint64_t)(svc) & 0xff) << 8) | ((cmd) & 0xff))
""",
     """/* For bsd/dev/pandora_gpu.c: an SCM call, results in a0 and a1. */
void pandora_scm_call(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t *a0,
    uint64_t *a1);

void
pandora_scm_call(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t *a0,
    uint64_t *a1)
{
	struct scm_res r;
	scm_smc(fn, arginfo, x2, x3, 0, 0, &r);
	*a0 = r.a0;
	*a1 = r.a1;
}

#define SCM_FN(svc, cmd)  ((1ULL << 30) | (2ULL << 24) | (((uint64_t)(svc) & 0xff) << 8) | ((cmd) & 0xff))
"""),
])
print('patched')
