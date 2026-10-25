// XNU payload (tools/mkpayload.py output) linked into the shim image.
// S-Boot loads the whole kernel image, so no ramdisk handling is needed.
// Build with -DPAYLOAD='"path/to/payload.bin"'; without it the section is
// empty and the shim runs its diagnostics instead.
        .section .payload, "a"
        .balign 16
        .globl payload_start
payload_start:
#ifdef PAYLOAD
        .incbin PAYLOAD
#endif
        .balign 16
        .quad   0, 0
