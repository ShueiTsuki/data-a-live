/* The stock Android kernel (prep.py: the Image from the stock boot_b, byte for byte). */
.section .linux, "a"
.balign 4096
.incbin LINUX
