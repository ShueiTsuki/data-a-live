/* The XNU payload (prep.py: shim payload with everyday boot-args) and the stock Android
 * kernel (prep.py: the Image from the stock boot_b, byte for byte). */
.section .payload, "a"
.balign 4096
.incbin PAYLOAD
.section .linux, "a"
.balign 4096
.incbin LINUX
