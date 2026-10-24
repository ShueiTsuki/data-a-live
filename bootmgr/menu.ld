ENTRY(_head)
SECTIONS {
	. = 0;
	.text : { *(.text.head) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	. = ALIGN(16);
	_bss_start = .;
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	_bss_end = .;
	. += 0x8000;
	_stack_top = .;
	. = ALIGN(4096);
	.config : { KEEP(*(.config)) }
	. = ALIGN(0x200000);
	_linux_start = .;
	.linux : { *(.linux) }
	_linux_end = .;
	_image_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
