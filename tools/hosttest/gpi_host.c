/*
 * Host tests for the GPI TRE and event encoders in wsl/pandora_gpi.h. Expected words are what
 * Linux 6.6 gpi_create_i2c_tre builds for the same request (bit positions from gpi.c's TRE_*
 * masks): the write-then-read split of i2c-qcom-geni geni_i2c_gpi_xfer, 400 kHz table entry.
 */
#include <stdio.h>
#include "../../wsl/pandora_gpi.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int
same(PgpiTre t, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (t.w[0] == a && t.w[1] == b && t.w[2] == c && t.w[3] == d) {
		return 1;
	}
	printf("  got %08x %08x %08x %08x, want %08x %08x %08x %08x\n", t.w[0], t.w[1], t.w[2], t.w[3], a, b, c, d);
	return 0;
}

int
main(void)
{
	/* {KHZ(400), clk_div 2, t_high 5, t_low 12, t_cycle 24}, TX and RX packing */
	const PgpiI2cClock c400 = {2, 5, 12, 24};
	CHECK(same(pgpi_i2c_config0(&c400), 12u | 5u << 8 | 24u << 16 | 1u << 24 | 1u << 25, 0, 2, 0x22u << 16 | 1u));
	/* message 0: write 1 byte to 0x55, a read follows (stretch), chained to its DMA TRE */
	CHECK(same(pgpi_i2c_go(PGPI_I2C_WRITE, 0x55, 1, 1, 0), 1u | 0x55u << 8 | 1u << 26, 0, 1, 0x20u << 16 | 1u));
	CHECK(same(pgpi_dma(0x123456789ull, 1), 0x23456789u, 1, 1, 0x10u << 16 | 1u << 9));
	/* message 1: read 2 bytes, linked to the RX channel's DMA TRE */
	CHECK(same(pgpi_i2c_go(PGPI_I2C_READ, 0x55, 0, 2, 1), 2u | 0x55u << 8, 0, 2, 0x20u << 16 | 1u << 11));
	/* Android's own read of the gauge (gpicheck raw\a94000_i2c.log): config, GO write with the command
	 * byte carried in an immediate TRE, then GO read of 2 bytes linked to RX */
	const PgpiI2cClock android = {2, 5, 11, 22};
	const uint8_t cmd = 0x70;
	CHECK(same(pgpi_i2c_config0(&android), 0x0316050bu, 0, 2, 0x00220001u));
	CHECK(same(pgpi_i2c_go(PGPI_I2C_WRITE, 0x55, 1, 0, 0), 0x04005501u, 0, 0, 0x00200001u));
	CHECK(same(pgpi_imm(&cmd, 1), 0x70u, 0, 1, 0x00110200u));
	CHECK(same(pgpi_i2c_go(PGPI_I2C_READ, 0x55, 0, 2, 1), 0x00005502u, 0, 2, 0x00200800u));
	const uint8_t eight[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	CHECK(same(pgpi_imm(eight, 8), 0x04030201u, 0x08070605u, 8, 0x00110200u));
	/* fields kept to their widths */
	CHECK(same(pgpi_i2c_go(0x3f, 0xff, 0, 0x1000000, 1), 0x1fu | 0x7fu << 8, 0, 0, 0x20u << 16 | 1u << 11));
	/* a transfer-complete event: TRE address, length, code EOT, channel 1 */
	const uint32_t ev[4] = {0x80001000u, 0x1u, 2u | 2u << 24, 0x22u << 16 | 1u << 24};
	const PgpiEvent e = pgpi_event(ev);
	CHECK(e.tre == 0x180001000ull && e.length == 2 && e.code == PGPI_CODE_EOT && e.type == PGPI_EV_XFER_DONE &&
	    e.chid == 1 && e.status == 0);
	printf("%s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures != 0;
}
