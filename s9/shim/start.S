// S9 shim entry (T2+): Image header, early hardware setup, stack, vectors, then C.
// Position-independent: only PC-relative addressing (adr / -mcmodel=tiny), so it runs
// wherever S-Boot loads it.

        .equ WDT0,      0x10050000      // watchdog_cl0 WTCON
        .equ WDT1,      0x10060000      // watchdog_cl1 WTCON
        .equ DECON_TRIG,0x16030070      // DECON_F HW_SW_TRIG_CONTROL

        .section .text.boot, "ax"
        .globl _start
_start:
        b       entry                   // code0
        .word   0                       // code1
        .quad   0x80000                 // text_offset
        .quad   _end - _start           // image_size
        .quad   0xa                     // flags: LE, 4K, anywhere
        .quad   0, 0, 0
        .ascii  "ARM\x64"
        .word   0

entry:
        msr     daifset, #0xf
        mrs     x9, mpidr_el1
        and     x9, x9, #0xffffff
        cbnz    x9, park

        // keep the four entry registers for C
        mov     x19, x0
        mov     x20, x1
        mov     x21, x2
        mov     x22, x3

        // watchdogs off, display refresh on (both proven by T0)
        ldr     x9, =WDT0
        str     wzr, [x9]
        ldr     x9, =WDT1
        str     wzr, [x9]
        ldr     x9, =DECON_TRIG
        ldr     w10, =0x1281
        str     w10, [x9]
        dsb     sy

        // stack inside the image
        adr     x9, stack_top
        mov     sp, x9

        // our own exception vectors, so a fault prints its cause instead of hanging
        adr     x9, vectors
        msr     vbar_el1, x9
        isb

        // zero .bss
        adr     x9, __bss_start
        adr     x10, __bss_end
1:      cmp     x9, x10
        b.hs    2f
        str     xzr, [x9], #8
        b       1b
2:
        mov     x0, x19
        mov     x1, x20
        mov     x2, x21
        mov     x3, x22
        adr     x4, _start              // where S-Boot actually loaded us
        bl      shim_main
park:
        wfe
        b       park

        .ltorg

// ---- exception vectors (16 entries x 0x80, 2 KiB aligned) ----
        .macro vec n
        .balign 0x80
        adr     x9, exc_stack_top
        mov     sp, x9
        mov     x0, #\n
        mrs     x1, esr_el1
        mrs     x2, elr_el1
        mrs     x3, far_el1
        mrs     x4, spsr_el1
        bl      shim_exception
        b       park
        .endm

        // IRQ taken from the current EL: save the caller-saved registers on the
        // interrupted stack, run shim_irq(), return. Used by the GIC/timer test.
        .macro irqvec
        .balign 0x80
        b       irq_common
        .endm

        .balign 2048
vectors:
        vec 0
        irqvec
        vec 2
        vec 3
        vec 4
        irqvec
        vec 6
        vec 7
        vec 8
        vec 9
        vec 10
        vec 11
        vec 12
        vec 13
        vec 14
        vec 15

irq_common:
        sub     sp, sp, #(24 * 8)
        stp     x0, x1, [sp, #0]
        stp     x2, x3, [sp, #16]
        stp     x4, x5, [sp, #32]
        stp     x6, x7, [sp, #48]
        stp     x8, x9, [sp, #64]
        stp     x10, x11, [sp, #80]
        stp     x12, x13, [sp, #96]
        stp     x14, x15, [sp, #112]
        stp     x16, x17, [sp, #128]
        stp     x18, x29, [sp, #144]
        mrs     x0, elr_el1
        mrs     x1, spsr_el1
        stp     x30, x0, [sp, #160]
        str     x1, [sp, #176]
        bl      shim_irq
        ldr     x1, [sp, #176]
        ldp     x30, x0, [sp, #160]
        msr     elr_el1, x0
        msr     spsr_el1, x1
        ldp     x18, x29, [sp, #144]
        ldp     x16, x17, [sp, #128]
        ldp     x14, x15, [sp, #112]
        ldp     x12, x13, [sp, #96]
        ldp     x10, x11, [sp, #80]
        ldp     x8, x9, [sp, #64]
        ldp     x6, x7, [sp, #48]
        ldp     x4, x5, [sp, #32]
        ldp     x2, x3, [sp, #16]
        ldp     x0, x1, [sp, #0]
        add     sp, sp, #(24 * 8)
        eret

        // Stacks live in .data (zeros stored in the file) rather than .bss, so they are
        // part of what S-Boot loads and can't overlap whatever it put right after us.
        .section .data.stacks, "aw"
        .balign 16
        .space  64 * 1024
stack_top:
        .space  16 * 1024
exc_stack_top:
