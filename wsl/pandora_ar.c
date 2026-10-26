/*
 * AudioReach APM/PRM commands from a graph description (pandora_ar.h).
 */
#include "pandora_ar.h"

#define GPR_HEADER   24u
#define CMD_HEADER   16u
#define PARAM_HEADER 16u

static void
put(ArPacket *k, uint32_t v, unsigned bytes)
{
	if (k->bad || k->n + bytes > k->cap) {
		k->bad = 1;
		return;
	}
	for (unsigned i = 0; i < bytes; i++) {
		k->p[k->n++] = (uint8_t)(v >> (8 * i));
	}
}

static void
set32(ArPacket *k, uint32_t at, uint32_t v)
{
	if (!k->bad && at + 4 <= k->n) {
		k->p[at] = (uint8_t)v;
		k->p[at + 1] = (uint8_t)(v >> 8);
		k->p[at + 2] = (uint8_t)(v >> 16);
		k->p[at + 3] = (uint8_t)(v >> 24);
	}
}

static uint32_t
rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void
ar_u8(ArPacket *k, uint32_t v)
{
	put(k, v, 1);
}

void
ar_u16(ArPacket *k, uint32_t v)
{
	put(k, v, 2);
}

void
ar_u32(ArPacket *k, uint32_t v)
{
	put(k, v, 4);
}

void
ar_bytes(ArPacket *k, const void *b, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		put(k, ((const uint8_t *)b)[i], 1);
	}
}

void
ar_begin(ArPacket *k, uint8_t *out, uint32_t capacity, uint32_t src_port, uint32_t dst_port, uint32_t token,
    uint32_t opcode)
{
	k->p = out;
	k->n = 0;
	k->cap = capacity;
	k->cmd = k->param = 0;
	k->bad = 0;
	put(k, 0, 4);                                          /* version, header size, packet size: ar_end */
	put(k, AR_DOMAIN_ADSP | AR_DOMAIN_APPS << 8, 4);       /* destination, source domain */
	put(k, src_port, 4);
	put(k, dst_port, 4);
	put(k, token, 4);
	put(k, opcode, 4);
}

void
ar_cmd(ArPacket *k)
{
	k->cmd = k->n;
	put(k, 0, 4);          /* payload address lsw: in band */
	put(k, 0, 4);          /* msw */
	put(k, 0, 4);          /* memory map handle */
	put(k, 0, 4);          /* payload size: ar_end */
}

void
ar_param(ArPacket *k, uint32_t iid, uint32_t param_id)
{
	k->param = k->n;
	put(k, iid, 4);
	put(k, param_id, 4);
	put(k, 0, 4);          /* size: ar_param_end */
	put(k, 0, 4);          /* error code */
}

void
ar_param_end(ArPacket *k)
{
	while (!k->bad && (k->n - k->param) & 7u) {
		put(k, 0, 1);
	}
	set32(k, k->param + 8, k->n - k->param - PARAM_HEADER);
}

void
ar_param_end_unpadded(ArPacket *k)
{
	set32(k, k->param + 8, k->n - k->param - PARAM_HEADER);
}

uint32_t
ar_end(ArPacket *k)
{
	if (k->bad) {
		return 0;
	}
	if (k->cmd) {
		set32(k, k->cmd + 12, k->n - k->cmd - CMD_HEADER);
	}
	set32(k, 0, 0u | (GPR_HEADER / 4) << 4 | k->n << 8);    /* version 0 */
	return k->n;
}

uint32_t
ar_graph_open(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArGraph *g)
{
	ArPacket k;
	ar_begin(&k, out, capacity, port, AR_APM_IID, token, AR_GRAPH_OPEN);
	ar_cmd(&k);
	ar_param(&k, AR_APM_IID, AR_PARAM_SUBGRAPH);
	ar_u32(&k, g->n_subgraphs);
	for (unsigned i = 0; i < g->n_subgraphs; i++) {
		const ArSubgraph *s = &g->subgraphs[i];
		ar_u32(&k, s->id);
		ar_u32(&k, 3);
		ar_u32(&k, AR_SG_PROP_PERF);
		ar_u32(&k, 4);
		ar_u32(&k, s->perf);
		ar_u32(&k, AR_SG_PROP_DIRECTION);
		ar_u32(&k, 4);
		ar_u32(&k, s->direction);
		ar_u32(&k, AR_SG_PROP_SCENARIO);
		ar_u32(&k, 4);
		ar_u32(&k, s->scenario);
	}
	ar_param_end(&k);
	ar_param(&k, AR_APM_IID, AR_PARAM_CONTAINER);
	ar_u32(&k, g->n_containers);
	for (unsigned i = 0; i < g->n_containers; i++) {
		const ArContainer *c = &g->containers[i];
		ar_u32(&k, c->id);
		ar_u32(&k, 4);
		ar_u32(&k, AR_CONT_PROP_CAPS);
		ar_u32(&k, 8);
		ar_u32(&k, 1);
		ar_u32(&k, c->capability);
		ar_u32(&k, AR_CONT_PROP_POS);
		ar_u32(&k, 4);
		ar_u32(&k, c->position);
		ar_u32(&k, AR_CONT_PROP_STACK);
		ar_u32(&k, 4);
		ar_u32(&k, c->stack);
		ar_u32(&k, AR_CONT_PROP_DOMAIN);
		ar_u32(&k, 4);
		ar_u32(&k, c->domain);
	}
	ar_param_end(&k);
	/* one list per run of modules sharing a sub-graph and a container */
	ar_param(&k, AR_APM_IID, AR_PARAM_MODULES);
	unsigned lists = 0;
	for (unsigned i = 0; i < g->n_modules; i++) {
		lists += i == 0 || g->modules[i].subgraph != g->modules[i - 1].subgraph ||
		    g->modules[i].container != g->modules[i - 1].container;
	}
	ar_u32(&k, lists);
	for (unsigned i = 0; i < g->n_modules;) {
		unsigned j = i;
		while (j < g->n_modules && g->modules[j].subgraph == g->modules[i].subgraph &&
		    g->modules[j].container == g->modules[i].container) {
			j++;
		}
		ar_u32(&k, g->modules[i].subgraph);
		ar_u32(&k, g->modules[i].container);
		ar_u32(&k, j - i);
		for (unsigned m = i; m < j; m++) {
			ar_u32(&k, g->modules[m].module);
			ar_u32(&k, g->modules[m].instance);
		}
		i = j;
	}
	ar_param_end(&k);
	ar_param(&k, AR_APM_IID, AR_PARAM_MODULE_PROP);
	ar_u32(&k, g->n_modules);
	for (unsigned i = 0; i < g->n_modules; i++) {
		ar_u32(&k, g->modules[i].instance);
		ar_u32(&k, 1);
		ar_u32(&k, AR_MOD_PROP_PORTS);
		ar_u32(&k, 8);
		ar_u32(&k, g->modules[i].inputs);
		ar_u32(&k, g->modules[i].outputs);
	}
	ar_param_end(&k);
	ar_param(&k, AR_APM_IID, AR_PARAM_LINKS);
	ar_u32(&k, g->n_links);
	for (unsigned i = 0; i < g->n_links; i++) {
		ar_u32(&k, g->links[i].from);
		ar_u32(&k, g->links[i].from_port);
		ar_u32(&k, g->links[i].to);
		ar_u32(&k, g->links[i].to_port);
	}
	ar_param_end(&k);
	return ar_end(&k);
}

uint32_t
ar_graph_command(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t opcode,
    const uint32_t *subgraphs, unsigned n)
{
	ArPacket k;
	ar_begin(&k, out, capacity, port, AR_APM_IID, token, opcode);
	ar_cmd(&k);
	ar_param(&k, AR_APM_IID, AR_PARAM_SUBGRAPHS);
	ar_u32(&k, n);
	for (unsigned i = 0; i < n; i++) {
		ar_u32(&k, subgraphs[i]);
	}
	ar_param_end(&k);
	return ar_end(&k);
}

uint32_t
ar_mem_map(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint64_t address, uint32_t bytes)
{
	ArPacket k;
	ar_begin(&k, out, capacity, port, AR_APM_IID, token, AR_MEM_MAP);
	ar_u16(&k, AR_POOL_SHMEM8_4K);
	ar_u16(&k, 1);                                 /* one region */
	ar_u32(&k, 0);                                 /* property flags */
	ar_u32(&k, (uint32_t)address);
	ar_u32(&k, (uint32_t)(address >> 32));
	ar_u32(&k, (bytes + 4095u) & ~4095u);          /* the DSP takes whole 4 KiB pages */
	return ar_end(&k);
}

uint32_t
ar_mem_unmap(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t handle)
{
	ArPacket k;
	ar_begin(&k, out, capacity, port, AR_APM_IID, token, AR_MEM_UNMAP);
	ar_u32(&k, handle);
	return ar_end(&k);
}

uint32_t
ar_prm_clock(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t clock_id, uint32_t hz,
    uint32_t attribute, uint32_t root)
{
	ArPacket k;
	ar_begin(&k, out, capacity, port, AR_PRM_IID, token, hz ? AR_PRM_REQUEST : AR_PRM_RELEASE);
	ar_cmd(&k);
	ar_param(&k, AR_PRM_IID, AR_PRM_PARAM_HW_CLK);
	ar_u32(&k, 1);                                 /* one clock */
	ar_u32(&k, clock_id);
	if (hz) {
		ar_u32(&k, hz);
		ar_u32(&k, attribute);
		ar_u32(&k, root);
	}
	ar_param_end_unpadded(&k);
	return ar_end(&k);
}

int
ar_parse(const uint8_t *in, uint32_t length, ArAnswer *a)
{
	if (length < GPR_HEADER) {
		return -1;
	}
	const uint32_t w0 = rd32(in), hdr = (w0 >> 4 & 0xfu) * 4, size = w0 >> 8;
	if (hdr < GPR_HEADER || size < hdr || size > length) {
		return -1;
	}
	a->src_port = rd32(in + 8);
	a->dst_port = rd32(in + 12);
	a->token = rd32(in + 16);
	a->opcode = rd32(in + 20);
	a->payload = in + hdr;
	a->payload_bytes = size - hdr;
	for (unsigned i = 0; i < 4; i++) {
		a->w[i] = a->payload_bytes >= 4 * i + 4 ? rd32(a->payload + 4 * i) : 0;
	}
	return 0;
}
