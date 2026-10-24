/*
 * pandora boot menu entry (from shim/loader/start.S). ABL loads boot_b's kernel field as a
 * Linux arm64 Image and jumps here with the MMU off and x0 = flattened device tree. Behind
 * the code: the settings page (.config) and, 2 MiB in, the stock Android kernel (.linux);
 * mkboot.py appends the iOS slot and raises image_size over all of it plus the screen save
 * area. Any unexpected exception resets the phone through PSCI SYSTEM_RESET.
 */
.section .text.head, "ax"
.global _head
_head:
	b	_start
	.long	0
	.quad	0			/* text_offset */
	.quad	_image_end		/* image_size: raised by mkboot.py */
	.quad	0xa			/* flags: little-endian, 4K pages, anywhere in RAM */
	.quad	0, 0, 0
	.ascii	"ARM\x64"
	.long	0

.text
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* FDT */
	mrs	x20, CurrentEL
	lsr	x20, x20, #2		/* exception level number */
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	cmp	x20, #2
	b.eq	1f
	msr	vbar_el1, x1
	b	2f
1:	msr	vbar_el2, x1
2:	isb
	adr	x1, _bss_start
	adr	x2, _bss_end
3:	cmp	x1, x2
	b.hs	4f
	str	xzr, [x1], #8
	b	3b
4:	mov	x0, x19
	mov	x1, x20
	adr	x2, _head
	bl	menu_main
.global fatal
fatal:
	mov	x0, #9
	movk	x0, #0x8400, lsl #16	/* PSCI SYSTEM_RESET */
	smc	#0
5:	wfe
	b	5b

/* void jump_linux(uint64_t entry, uint64_t fdt): arm64 boot protocol, x0 = FDT, x1-x3 = 0,
 * MMU off, interrupts masked; for the Android kernel and the slot's Image alike. Nothing
 * here enabled a cache, so only the I-cache is cleaned. */
.global jump_linux
jump_linux:
	mov	x4, x0
	mov	x0, x1
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	dsb	sy
	ic	iallu
	dsb	sy
	isb
	br	x4

/* Exception vectors: only a synchronous exception at the current level with SP_ELx
 * (entry 4) can be recovered, and only inside safe_rd32. */
.balign 0x800
vectors:
.rept 4
	b	fatal
	.balign 0x80
.endr
	b	sync_fault
	.balign 0x80
.rept 11
	b	fatal
	.balign 0x80
.endr

sync_fault:
	stp	x0, x1, [sp, #-16]!
	adrp	x0, fault_fixup
	ldr	x1, [x0, :lo12:fault_fixup]
	cbz	x1, 9f
	str	xzr, [x0, :lo12:fault_fixup]
	mrs	x0, CurrentEL
	cmp	x0, #8
	b.eq	2f
	msr	elr_el1, x1
	mrs	x1, esr_el1
	b	3f
2:	msr	elr_el2, x1
	mrs	x1, esr_el2
3:	adrp	x0, fault_esr
	str	x1, [x0, :lo12:fault_esr]
	ldp	x0, x1, [sp], #16
	eret
9:	ldp	x0, x1, [sp], #16
	b	fatal

/* uint32_t safe_rd32(uint64_t pa, uint32_t *faulted): a synchronous abort on the read
 * returns 0xdeadfa17 and sets *faulted instead of resetting. An asynchronous SError
 * still resets the phone. */
.global safe_rd32
safe_rd32:
	adrp	x2, fault_fixup
	adr	x3, 4f
	str	x3, [x2, :lo12:fault_fixup]
	dsb	sy
	ldr	w0, [x0]
	dsb	sy
	str	xzr, [x2, :lo12:fault_fixup]
	str	wzr, [x1]
	ret
4:	mov	w3, #1
	str	w3, [x1]
	mov	w0, #0xfa17
	movk	w0, #0xdead, lsl #16
	ret

.bss
.balign 8
fault_fixup:	.quad 0
.global fault_esr
fault_esr:	.quad 0
