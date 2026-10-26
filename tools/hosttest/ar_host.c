/*
 * Host tests for wsl/pandora_ar.c (AudioReach APM/PRM commands from a graph description).
 *
 * The PRM clock request and release are checked byte for byte against the packets Android sends on
 * this phone (D:\DeepSeekHarness\prmcheck, verified from audio_prm_dlkm and machine_dlkm). With
 * -DSHENG (run.sh SHENG_DIR=...), the graph packets are also checked against UniXNU-sheng's
 * encoder, which its board's DSP accepts: the same graph through both must give the same bytes.
 * sheng's code is compiled in for the comparison only, under renamed symbols.
 */
#include <stdio.h>
#include <string.h>
#include "../../wsl/pandora_ar.h"

#ifdef SHENG
/* sheng's functions share names with ours: renamed here and when its file is compiled (run.sh) */
#define ArAnswer ShengArAnswer
#define ar_graph_command sheng_ar_graph_command
#define ar_map sheng_ar_map
#define ar_unmap sheng_ar_unmap
#define ar_write sheng_ar_write
#define ar_read sheng_ar_read
#define ar_prm_clock sheng_ar_prm_clock
#define ar_prm_clock_attr sheng_ar_prm_clock_attr
#define ar_prm_vote sheng_ar_prm_vote
#define ar_parse sheng_ar_parse
#include "ShengAudioReach.h"
#undef ArAnswer
#undef ar_graph_command
#undef ar_map
#undef ar_unmap
#undef ar_write
#undef ar_read
#undef ar_prm_clock
#undef ar_prm_clock_attr
#undef ar_prm_vote
#undef ar_parse
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint32_t
rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int
words_equal(const uint8_t *p, uint32_t n, const uint32_t *want, unsigned count)
{
	if (n != 4 * count) {
		printf("  length %u, want %u\n", n, 4 * count);
		return 0;
	}
	for (unsigned i = 0; i < count; i++) {
		if (rd32(p + 4 * i) != want[i]) {
			printf("  word %u: %#x, want %#x\n", i, rd32(p + 4 * i), want[i]);
			return 0;
		}
	}
	return 1;
}

static void
android_prm(void)
{
	uint8_t b[128];
	/* Android: source port 7, token 0x1000 (counter << 12) */
	uint32_t n = ar_prm_clock(b, sizeof(b), 7, 0x1000, 0x200, 3072000, 4, 0);
	static const uint32_t request[19] = {0x4c60, 0x302, 7, 2, 0x1000, 0x0100100f, 0, 0, 0, 0x24,
	                                     2, 0x0800102c, 0x14, 0, 1, 0x200, 3072000, 4, 0};
	CHECK(words_equal(b, n, request, 19));
	n = ar_prm_clock(b, sizeof(b), 7, 0x2000, 0x200, 0, 0, 0);
	static const uint32_t release[16] = {0x4060, 0x302, 7, 2, 0x2000, 0x01001010, 0, 0, 0, 0x18,
	                                     2, 0x0800102c, 8, 0, 1, 0x200};
	CHECK(words_equal(b, n, release, 16));
	/* memory map: {u16 pool, u16 regions, u32 flags, lsw, msw, size rounded to 4 KiB} */
	n = ar_mem_map(b, sizeof(b), 1, 9, 0x1b0000000ull, 0x1001);
	static const uint32_t map[11] = {0x2c60, 0x302, 1, 1, 9, 0x0100100c, 0x10003, 0, 0xb0000000u, 1, 0x2000};
	CHECK(words_equal(b, n, map, 11));
	ArAnswer a;
	CHECK(ar_parse(b, n, &a) == 0 && a.opcode == AR_MEM_MAP && a.payload_bytes == 20 && a.w[2] == 0xb0000000u);
	CHECK(ar_parse(b, 20, &a) == -1);
	/* a packet that does not fit */
	CHECK(ar_prm_clock(b, 40, 7, 1, 0x200, 3072000, 4, 0) == 0);
}

#ifdef SHENG
/* tail: sheng pads the memory map commands with 16 zero bytes Linux does not send (audioreach.c
 * builds them without a command header); our 44-byte map was accepted by this phone's DSP (spk1).
 * The packet sizes in word 0 differ by the tail; everything else must match and the tail be zero. */
static void
compare_tail(const char *what, const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb, uint32_t tail)
{
	int same = na > 4 && na + tail == nb && memcmp(a + 4, b + 4, na - 4) == 0 && a[0] == b[0] &&
	    rd32(a) >> 8 == na && rd32(b) >> 8 == nb;
	for (uint32_t i = na; same && i < nb; i++) {
		same = b[i] == 0;
	}
	printf("  %-22s ours %u bytes, sheng's %u: %s\n", what, na, nb,
	    same ? (tail ? "same, sheng's ends in zero padding" : "same") : "DIFFERENT");
	if (!same) {
		for (uint32_t i = 0; i < na && i < nb; i += 4) {
			if (rd32(a + i) != rd32(b + i)) {
				printf("    first difference at byte %u: %#x vs %#x\n", i, rd32(a + i), rd32(b + i));
				break;
			}
		}
	}
	CHECK(same);
}

static void
compare(const char *what, const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb)
{
	compare_tail(what, a, na, b, nb, 0);
}

static void
against_sheng(void)
{
	uint8_t ours[512], theirs[512];
	/* sheng's secondary-MI2S speaker device graph and its playback stream graph, as data */
	static const ArSubgraph dev_sg = {0x4005, 2, 1, 1}, play_sg = {0x4001, 2, 2, 1};
	static const ArContainer dev_c = {0x4005, 0x0B001001, 1, 8192, 2}, play_c = {0x4001, 0x0B001001, 1, 8192, 2};
	static const ArModule dev_m[] = {{0x4005, 0x4005, 0x0700100A, 0x6051, 1, 0}};
	static const ArModule play_m[] = {{0x4001, 0x4001, 0x07001000, 0x6001, 0, 1}, {0x4001, 0x4001, 0x07001005, 0x6002, 1, 1},
	                                  {0x4001, 0x4001, 0x07001003, 0x6003, 1, 1}};
	static const ArLink play_l[] = {{0x6001, 1, 0x6002, 2}, {0x6002, 1, 0x6003, 2}, {0x6003, 1, 0x6051, 2}};
	const ArGraph dev = {&dev_sg, 1, &dev_c, 1, dev_m, 1, 0, 0};
	const ArGraph play = {&play_sg, 1, &play_c, 1, play_m, 3, play_l, 3};
	compare("speaker device open", ours, ar_graph_open(ours, sizeof(ours), 1, 5, &dev),
	    theirs, ar_open_speaker_device(theirs, sizeof(theirs), 1, 5));
	compare("playback stream open", ours, ar_graph_open(ours, sizeof(ours), 1, 6, &play),
	    theirs, ar_open_playback_stream(theirs, sizeof(theirs), 1, 6));
	const uint32_t sg = 0x4001;
	compare("graph start", ours, ar_graph_command(ours, sizeof(ours), 1, 7, AR_GRAPH_START, &sg, 1),
	    theirs, sheng_ar_graph_command(theirs, sizeof(theirs), 1, 7, AR_OP_GRAPH_START, 0x4001));
	compare_tail("memory map", ours, ar_mem_map(ours, sizeof(ours), 1, 8, 0x1b0000000ull, 0x10000),
	    theirs, sheng_ar_map(theirs, sizeof(theirs), 1, 8, 0x1b0000000ull, 0x10000), 16);
	compare_tail("memory unmap", ours, ar_mem_unmap(ours, sizeof(ours), 1, 9, 0x1234),
	    theirs, sheng_ar_unmap(theirs, sizeof(theirs), 1, 9, 0x1234), 16);
}
#endif

int
main(void)
{
	android_prm();
#ifdef SHENG
	against_sheng();
#endif
	printf("%s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures != 0;
}
