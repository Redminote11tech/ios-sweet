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

        // Current EL, SPx, synchronous: a fault on the load/store inside
        // safe_rd32/safe_wr32 (probing registers that may not exist) returns
        // to the failure path instead of stopping the shim.
        .macro vecsafe
        .balign 0x80
        mrs     x9, elr_el1
        adr     x10, safe_ld
        cmp     x9, x10
        b.eq    1f
        adr     x10, safe_st
        cmp     x9, x10
        b.eq    1f
        b       vec4_fatal
1:      adr     x9, safe_fail
        msr     elr_el1, x9
        eret
        .endm

        .balign 2048
vectors:
        vec 0
        irqvec
        vec 2
        vec 3
        vecsafe
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

vec4_fatal:
        adr     x9, exc_stack_top
        mov     sp, x9
        mov     x0, #4
        mrs     x1, esr_el1
        mrs     x2, elr_el1
        mrs     x3, far_el1
        mrs     x4, spsr_el1
        bl      shim_exception
        b       park

// u32 safe_rd32(u64 addr): 0xDEADBEEF and safe_faults++ if the read aborts
        .globl safe_rd32
safe_rd32:
safe_ld:
        ldr     w0, [x0]
        ret
// void safe_wr32(u64 addr, u32 v)
        .globl safe_wr32
safe_wr32:
safe_st:
        str     w1, [x0]
        ret
safe_fail:
        adr     x9, safe_faults
        ldr     w10, [x9]
        add     w10, w10, #1
        str     w10, [x9]
        movz    w0, #0xbeef
        movk    w0, #0xdead, lsl #16
        ret

// SMP probe (t8): PSCI CPU_ON target. x0 = context id (slot). Stores
// 0x5EC0_0000 | CurrentEL << 8 | (MPIDR & 0xff..) in mailbox[x0], then parks.
// Position independent and self-contained (the copy to high RAM takes
// smp_entry .. smp_entry_end, literal included).
        .balign 8
        .globl smp_entry, smp_entry_end
smp_entry:
        // Self-reporting secondary: writes a hello to its mailbox slot (by
        // MPIDR: aff0 + 4*aff1), paints a bar on the framebuffer (green at
        // EL1, yellow at EL2), optionally chain-starts the core named in the
        // table at mbox+0x200, then counts forever with a moving white block.
        // Position independent, MMU off, no stack.
        mrs     x1, mpidr_el1
        and     x1, x1, #0xffff
        and     x4, x1, #0xff
        lsr     x5, x1, #8
        and     x5, x5, #0xff
        add     x4, x4, x5, lsl #2          // slot
        mrs     x2, CurrentEL
        orr     x1, x1, x2, lsl #14
        movz    w3, #0x5ec0, lsl #16
        orr     x1, x1, x3
        ldr     x6, smp_mbox
        add     x6, x6, x4, lsl #4          // our 16-byte slot
        str     x1, [x6]
        mrs     x1, mpidr_el1
        str     w1, [x6, #4]                // raw MPIDR_EL1 (low 32 bits)
        dsb     sy
        // bar: rows 2300 + slot*80, 40 rows, full width (below the text)
        ldr     x7, smp_fb
        mov     x8, #5760
        mov     x9, #2300
        add     x9, x9, x4, lsl #6
        add     x9, x9, x4, lsl #4          // slot*80
        mul     x9, x9, x8
        add     x7, x7, x9
        mov     x17, x7                     // keep the bar origin
        movz    w10, #0xff00                // green: EL1
        cmp     x2, #4
        b.eq    1f
        movk    w10, #0x00ff, lsl #16       // yellow: anything else
1:      mov     x11, #40
2:      mov     x12, #1440
        mov     x13, x7
3:      str     w10, [x13], #4
        subs    x12, x12, #1
        b.ne    3b
        add     x7, x7, x8
        subs    x11, x11, #1
        b.ne    2b
        dsb     sy
        // chain-start the next core if the table says so
        ldr     x2, smp_mbox
        add     x14, x2, #0x200
        ldr     w1, [x14, x4, lsl #2]
        cbz     w1, 9f
        movz    w15, #0xca11
        str     w15, [x6, #8]
        dsb     sy
        movz    x0, #0x0003
        movk    x0, #0xc400, lsl #16
        adr     x2, smp_entry
        mov     x3, #0
        smc     #0
        movz    w15, #0xd0e, lsl #16
        orr     w15, w15, w0                // 0x0d0e_rc: chain CPU_ON returned
        str     w15, [x6, #8]
        dsb     sy
        // alive loop: counter in word 3, white block walking along the bar
9:      mov     x15, #0
10:     add     x15, x15, #1
        str     w15, [x6, #12]
        and     x12, x15, #0x3f             // 64 positions x 20 px
        mov     x13, #20
        mul     x12, x12, x13
        add     x13, x17, x12, lsl #2
        mov     x11, #8
11:     mov     x12, #16
        mov     x14, x13
        movz    w10, #0xffff
        movk    w10, #0x00ff, lsl #16
12:     str     w10, [x14], #4
        subs    x12, x12, #1
        b.ne    12b
        add     x13, x13, x8
        subs    x11, x11, #1
        b.ne    11b
        dsb     sy
        mov     x16, #0x400000
13:     subs    x16, x16, #1
        b.ne    13b
        b       10b
        .balign 8
smp_mbox:
        .quad   0xef300000
smp_fb:
        .quad   0xcc000000
smp_entry_end:

// big_entry .. big_entry_end: big-core probe (t9). x0 = PSCI context id
// (0..3) picks a fixed mailbox slot at big_mb + 16*ctx and a fixed magenta
// bar at rows 1400 + 120*ctx: nothing depends on MPIDR. Word 0 = 0xb16c0000
// | CurrentEL, word 1 = MPIDR_EL1, word 2 = counter. MMU off, no stack.
        .balign 8
        .globl big_entry, big_entry_end
big_entry:
        and     x10, x0, #3
        mrs     x1, CurrentEL
        ldr     x6, big_mb
        add     x6, x6, x10, lsl #4
        movz    w3, #0xb16c, lsl #16
        orr     w3, w3, w1
        str     w3, [x6]
        mrs     x1, mpidr_el1
        str     w1, [x6, #4]
        dsb     sy
        ldr     x7, big_fb
        mov     x8, #5760
        mov     x9, #1400
        mov     x12, #120
        madd    x9, x10, x12, x9
        mul     x9, x9, x8
        add     x7, x7, x9
        movz    w13, #0x00ff
        movk    w13, #0x00ff, lsl #16           // magenta
        mov     x11, #60
1:      mov     x12, #1440
        mov     x14, x7
2:      str     w13, [x14], #4
        subs    x12, x12, #1
        b.ne    2b
        add     x7, x7, x8
        subs    x11, x11, #1
        b.ne    1b
        dsb     sy
        mov     x15, #0
3:      add     x15, x15, #1
        str     w15, [x6, #8]
        dsb     sy
        mov     x16, #0x100000
4:      subs    x16, x16, #1
        b.ne    4b
        b       3b
        .balign 8
big_mb:
        .quad   0xef300800
big_fb:
        .quad   0xcc000000
big_entry_end:

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
        .balign 4
        .globl safe_faults
safe_faults:
        .word   0
        .balign 16
        .space  64 * 1024
stack_top:
        .space  16 * 1024
exc_stack_top:
