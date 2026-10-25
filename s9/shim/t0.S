// S9 shim, test T0 — watchdogs off, red screen, EL indicator, heartbeat.
// Galaxy S9 SM-G960F (Exynos 9810). Booted by S-Boot as if it were an arm64 Linux Image.
//
// What you should see once it boots:
//   - whole screen red (if it is BLUE instead, the pixel format has red/blue swapped: also useful)
//   - a square near the top-left showing the exception level we were entered at:
//       green = EL1 (expected), blue = EL2, white = anything else
//   - a white bar near the top that grows one step per second (about 140 s to cross the screen)
//   - the phone does NOT reboot after 20-30 s (that proves the watchdogs are off)

        .equ WDT0,      0x10050000      // watchdog_cl0, WTCON at +0
        .equ WDT1,      0x10060000      // watchdog_cl1, WTCON at +0
        .equ DECON_F,   0x16030000      // display controller (decon_f in the device tree)
        .equ FB,        0xCC000000      // s3cfb.bootloaderfb from /chosen/bootargs
        .equ FB_W,      1440
        .equ FB_H,      2960
        .equ STRIDE,    (FB_W * 4)      // guess: 32 bpp, no padding (T1 confirms)
        .equ FB_SIZE,   (STRIDE * FB_H)

        .equ RED,       0xFFFF0000      // ARGB8888 / XRGB8888
        .equ GREEN,     0xFF00FF00
        .equ BLUE,      0xFF0000FF
        .equ WHITE,     0xFFFFFFFF

        .text
        .globl _start
_start:
        // ---- arm64 Linux Image header (64 bytes) ----
        b       start                   // code0
        .word   0                       // code1
        .quad   0x80000                 // text_offset
        .quad   _end - _start           // image_size
        .quad   0xa                     // flags: little-endian, 4K pages, place anywhere
        .quad   0, 0, 0                 // res2-4
        .ascii  "ARM\x64"               // magic
        .word   0                       // res5 (no PE header)

start:
        msr     daifset, #0xf           // mask all interrupts
        mov     x20, x0                 // keep the FDT pointer (unused in T0)

        // Only the boot CPU continues; anything else parks.
        mrs     x1, mpidr_el1
        and     x1, x1, #0xffffff
        cbnz    x1, park

        // ---- 1. watchdogs off: WTCON = 0 (clears enable + reset-enable) ----
        ldr     x2, =WDT0
        str     wzr, [x2]
        ldr     x2, =WDT1
        str     wzr, [x2]
        dsb     sy

        // ---- 1b. let the display refresh from the framebuffer ----
        // DECON_F runs the panel in command mode; S-Boot leaves frame updates off.
        // Setting HW_SW_TRIG_CONTROL (DECON_F + 0x70) to 0x1281 turns on hardware
        // (TE-driven) triggering, the same write uniLoader does for starlte.
        ldr     x2, =DECON_F + 0x70
        ldr     w3, =0x1281
        str     w3, [x2]
        dsb     sy

        // ---- 2. fill the whole framebuffer red ----
        ldr     x0, =FB
        ldr     x1, =FB_SIZE
        ldr     w2, =RED
        bl      fill

        // ---- 3. EL indicator: 240x240 square at (60, 300) ----
        mrs     x3, CurrentEL
        lsr     x3, x3, #2
        and     x3, x3, #3
        ldr     w2, =WHITE
        cmp     x3, #1
        b.ne    1f
        ldr     w2, =GREEN
1:      cmp     x3, #2
        b.ne    2f
        ldr     w2, =BLUE
2:      mov     x0, #60                 // x
        mov     x1, #300                // y
        mov     x3, #240                // w
        mov     x4, #240                // h
        bl      rect

        // ---- 4. heartbeat: one 8x40 white block per second, 10 px apart, at y = 100 ----
        mrs     x21, cntfrq_el0
        cbnz    x21, 3f
        ldr     x21, =26000000          // S9 arch timer is 26 MHz (from the device tree)
3:      isb
        mrs     x22, cntpct_el0         // time of the last tick
        mov     x23, #0                 // seconds shown
tick:
        isb
        mrs     x9, cntpct_el0
        sub     x10, x9, x22
        cmp     x10, x21
        b.lo    tick
        add     x22, x22, x21

        mov     x0, #10
        mul     x0, x0, x23             // x = 10 * seconds
        add     x0, x0, #20
        mov     x1, #100                // y
        mov     x3, #8                  // w
        mov     x4, #40                 // h
        ldr     w2, =WHITE
        bl      rect

        add     x23, x23, #1
        cmp     x23, #140               // 140 blocks fit in 1440 px; then start over
        b.lo    tick
        mov     x23, #0
        // restart the bar on a fresh red strip so the count stays readable
        ldr     x0, =FB + 100 * STRIDE
        ldr     x1, =40 * STRIDE
        ldr     w2, =RED
        bl      fill
        b       tick

park:
        wfe
        b       park

// fill(x0 = start, x1 = bytes, w2 = pixel) — bytes must be a multiple of 64
fill:
        orr     x2, x2, x2, lsl #32
        add     x1, x0, x1
        mov     x5, x0
4:      stp     x2, x2, [x0], #16
        stp     x2, x2, [x0], #16
        stp     x2, x2, [x0], #16
        stp     x2, x2, [x0], #16
        cmp     x0, x1
        b.lo    4b
        // clean to memory in case S-Boot left the D-cache on (harmless if it is off)
5:      dc      cvac, x5
        add     x5, x5, #64
        cmp     x5, x1
        b.lo    5b
        dsb     sy
        ret

// rect(x0 = x, x1 = y, w2 = pixel, x3 = w, x4 = h)
rect:
        ldr     x6, =FB
        ldr     x7, =STRIDE
        madd    x6, x1, x7, x6          // row start = FB + y * STRIDE
        add     x6, x6, x0, lsl #2      // + x * 4
6:      mov     x8, x6
        mov     x9, x3
7:      str     w2, [x8], #4
        subs    x9, x9, #1
        b.ne    7b
        // clean this row's cache lines
        mov     x10, x6
8:      dc      cvac, x10
        add     x10, x10, #64
        cmp     x10, x8
        b.lo    8b
        add     x6, x6, x7
        subs    x4, x4, #1
        b.ne    6b
        dsb     sy
        ret

        .ltorg
        .balign 4096
_end:
