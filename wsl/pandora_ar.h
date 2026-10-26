/*
 * Commands for the audio DSP's graph manager (APM) and resource manager (PRM), Qualcomm AudioReach
 * over GPR, built from a description of the graph instead of per-board code: sub-graphs,
 * containers, modules and links go in, the GPR packets come out. Layouts as the AudioReach API
 * (Linux sound/soc/qcom/qdsp6/audioreach.h): a command header {payload address, memory map handle,
 * payload size} followed by parameter blocks {instance, parameter id, size, error code, body padded
 * to 8 bytes}. The board's graph (modules, parameters) is data for the caller.
 * No allocation, no I/O, no waits.
 */
#ifndef PANDORA_AR_H
#define PANDORA_AR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPR addressing */
#define AR_DOMAIN_ADSP        2u
#define AR_DOMAIN_APPS        3u
#define AR_APM_IID            1u
#define AR_PRM_IID            2u

/* APM commands and answers */
#define AR_GRAPH_OPEN         0x01001000u
#define AR_GRAPH_PREPARE      0x01001001u
#define AR_GRAPH_START        0x01001002u
#define AR_GRAPH_STOP         0x01001003u
#define AR_GRAPH_CLOSE        0x01001004u
#define AR_SET_CFG            0x01001006u
#define AR_GET_CFG            0x01001007u
#define AR_MEM_MAP            0x0100100Cu
#define AR_MEM_UNMAP          0x0100100Du
#define AR_BASIC_RESULT       0x02001005u   /* {command, status} */
#define AR_MEM_MAP_DONE       0x02001001u   /* {handle} */
#define AR_POOL_SHMEM8_4K     3u

/* PRM */
#define AR_PRM_REQUEST        0x0100100Fu
#define AR_PRM_RELEASE        0x01001010u
#define AR_PRM_REQUEST_DONE   0x02001002u
#define AR_PRM_RELEASE_DONE   0x02001003u
#define AR_PRM_PARAM_HW_CLK   0x0800102Cu

/* Graph description parameters and properties */
#define AR_PARAM_CONTAINER    0x08001000u
#define AR_PARAM_SUBGRAPH     0x08001001u
#define AR_PARAM_MODULES      0x08001002u
#define AR_PARAM_MODULE_PROP  0x08001003u
#define AR_PARAM_LINKS        0x08001004u
#define AR_PARAM_SUBGRAPHS    0x08001005u
#define AR_SG_PROP_PERF       0x0800100Eu
#define AR_SG_PROP_DIRECTION  0x0800100Fu
#define AR_SG_PROP_SCENARIO   0x08001010u
#define AR_CONT_PROP_CAPS     0x08001011u
#define AR_CONT_PROP_POS      0x08001012u
#define AR_CONT_PROP_STACK    0x08001013u
#define AR_CONT_PROP_DOMAIN   0x08001014u
#define AR_MOD_PROP_PORTS     0x08001015u

typedef struct {
	uint32_t id, perf, direction, scenario;
} ArSubgraph;

typedef struct {
	uint32_t id, capability, position, stack, domain;
} ArContainer;

typedef struct {
	uint32_t subgraph, container, module, instance, inputs, outputs;
} ArModule;

typedef struct {
	uint32_t from, from_port, to, to_port;
} ArLink;

typedef struct {
	const ArSubgraph *subgraphs;
	unsigned n_subgraphs;
	const ArContainer *containers;
	unsigned n_containers;
	const ArModule *modules;           /* grouped by sub-graph and container, in order */
	unsigned n_modules;
	const ArLink *links;
	unsigned n_links;
} ArGraph;

/* A packet under construction; bad is set when it outgrows the buffer. */
typedef struct {
	uint8_t *p;
	uint32_t n, cap, cmd, param;
	int bad;
} ArPacket;

/* Starts a packet: GPR header to dst_port from src_port with token and opcode. */
void ar_begin(ArPacket *k, uint8_t *out, uint32_t capacity, uint32_t src_port, uint32_t dst_port, uint32_t token,
    uint32_t opcode);
/* The in-band command header of APM_CMD_* with parameter blocks (its size is patched in ar_end). */
void ar_cmd(ArPacket *k);
/* A parameter block for instance iid, its body written with ar_u8/16/32/bytes, closed by ar_param_end. */
void ar_param(ArPacket *k, uint32_t iid, uint32_t param_id);
void ar_param_end(ArPacket *k);
/* The same without padding the body to 8 bytes: the PRM's resource requests (Linux q6prm, and the
 * 76-byte clock request Android sends) are not padded. */
void ar_param_end_unpadded(ArPacket *k);
void ar_u8(ArPacket *k, uint32_t v);
void ar_u16(ArPacket *k, uint32_t v);
void ar_u32(ArPacket *k, uint32_t v);
void ar_bytes(ArPacket *k, const void *b, uint32_t n);
/* Finishes the packet: sizes patched; returns its length or 0 when it did not fit. */
uint32_t ar_end(ArPacket *k);

/* Whole packets */
uint32_t ar_graph_open(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArGraph *g);
/* opcode: AR_GRAPH_PREPARE, _START, _STOP or _CLOSE, for the sub-graphs listed */
uint32_t ar_graph_command(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t opcode,
    const uint32_t *subgraphs, unsigned n);
uint32_t ar_mem_map(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint64_t address,
    uint32_t bytes);
uint32_t ar_mem_unmap(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t handle);
/* PRM audio hardware clock: hz != 0 requests it (attribute, root), hz == 0 releases it. */
uint32_t ar_prm_clock(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t clock_id, uint32_t hz,
    uint32_t attribute, uint32_t root);

/* An answer's header and its first payload words. */
typedef struct {
	uint32_t opcode, token, src_port, dst_port, payload_bytes;
	uint32_t w[4];                 /* the first payload words (0 past the end) */
	const uint8_t *payload;
} ArAnswer;
/* 0, or -1 when the bytes are not a whole GPR packet. */
int ar_parse(const uint8_t *in, uint32_t length, ArAnswer *a);

#ifdef __cplusplus
}
#endif

#endif
