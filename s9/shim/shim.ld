/* Flat image linked at 0; all code is PC-relative so the real load address doesn't matter. */
ENTRY(_start)
SECTIONS
{
    . = 0;
    .text   : { *(.text.boot) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    __bss_start = .;
    .bss    : { *(.bss .bss.* COMMON) }
    __bss_end = .;
    /* Last, so every other symbol stays within adr range (+-1 MiB) of the code. */
    . = ALIGN(16);
    .payload : { *(.payload) }
    . = ALIGN(4096);
    _end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
