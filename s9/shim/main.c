// S9 shim: XNU loader for the Galaxy S9 (SM-G960F, Exynos 9810).
//
// If an XNU payload (tools/mkpayload.py, magic "SWEETXNU") is linked into the
// image, load it into the 2 GiB bank at 0x880000000 and jump to it, acting as
// iBoot. Without a payload: test T2+T3, a text console with the facts the XNU
// port needs.
//
// Prints: load address and entry registers, exception level and MMU/cache state, CPU ID
// and feature registers (16K page support), the device tree's memory banks / initrd /
// framebuffer bootarg, then probes the 2 GiB bank at 0x880000000. Afterwards a large
// seconds counter runs in the lower half of the screen.
//
// Runs with the MMU off, so every access is Device memory: no unaligned accesses
// (built with -mstrict-align) and no pointer-containing initialised data (built
// position-independent with -mcmodel=tiny).

#include <stdint.h>
#include "font.h"

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

// ---------------------------------------------------------------- framebuffer
#ifdef HOST    // host-side preview build (see test_host.c); not used on the phone
extern u32 host_fb[];
#define FB      ((volatile u32 *)host_fb)
#else
#define FB      ((volatile u32 *)0xCC000000UL)
#endif
#define FB_W    1440
#define FB_H    2960
#define STRIDE  1440            // in pixels (5760 bytes), confirmed by T0
#define SCALE   2               // 16x32 font drawn at 32x64
#define CW      (FONT_W * SCALE)
#define CH      (FONT_H * SCALE)
#define COLS    (FB_W / CW)     // 45

#define BG      0xFF101828
#define FG      0xFFE8E8E8
#define DIM     0xFF8890A0
#define OK      0xFF40E060
#define BAD     0xFFFF5050
#define HEAD    0xFF60B0FF
#define WARN    0xFFFFD040

static int cur_x, cur_y = 1;
static u32 cur_fg = FG;

static void rect(int x, int y, int w, int h, u32 c)
{
    if (y < 0 || x < 0 || y + h > FB_H || x + w > FB_W)
        return;                         // clip: never write outside the framebuffer
    for (int j = 0; j < h; j++) {
        volatile u32 *p = FB + (u64)(y + j) * STRIDE + x;
        for (int i = 0; i < w; i++)
            p[i] = c;
    }
}

static void glyph(int col, int row, char ch, u32 fg, u32 bg)
{
    if (ch < 32 || ch > 126)
        ch = '?';
    const unsigned short *g = font[ch - 32];
    int x0 = col * CW + 24, y0 = row * CH;      // 24 px margin for the rounded corners
    for (int y = 0; y < FONT_H; y++) {
        unsigned short bits = g[y];
        for (int s = 0; s < SCALE; s++) {
            volatile u32 *p = FB + (u64)(y0 + y * SCALE + s) * STRIDE + x0;
            for (int x = 0; x < FONT_W; x++) {
                u32 c = (bits & (0x8000 >> x)) ? fg : bg;
                for (int t = 0; t < SCALE; t++)
                    *p++ = c;
            }
        }
    }
}

static void putch(char c)
{
    if (c == '\n' || cur_x >= COLS - 1) {
        cur_x = 0;
        cur_y++;
        if (cur_y >= FB_H / CH - 1)
            cur_y = 1;                  // wrap to the top instead of running off the screen
        if (c == '\n')
            return;
    }
    glyph(cur_x++, cur_y, c, cur_fg, BG);
}

static void puts_(const char *s)
{
    while (*s)
        putch(*s++);
}

static void color(u32 c) { cur_fg = c; }

static void hex(u64 v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        putch("0123456789abcdef"[(v >> (i * 4)) & 0xf]);
}

static void hexv(u64 v)                 // shortest form, at least 1 digit
{
    int d = 1;
    while (d < 16 && (v >> (d * 4)))
        d++;
    puts_("0x");
    hex(v, d);
}

static void dec(u64 v)
{
    char b[21];
    int i = 0;
    do {
        b[i++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (i)
        putch(b[--i]);
}

static void kv(const char *k, u64 v)
{
    color(DIM);
    puts_(k);
    color(FG);
    hexv(v);
    putch('\n');
}

#ifdef HOST
u64 host_sysreg(const char *name);
#define SYSREG(name) host_sysreg(#name)
#else
#define SYSREG(name) ({ u64 _v; __asm__ volatile("mrs %0, " #name : "=r"(_v)); _v; })
#endif

// ---------------------------------------------------------------- device tree
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static int prefix(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}

static u64 cells(const u8 *p, int n)
{
    u64 v = 0;
    for (int i = 0; i < n; i++)
        v = v << 32 | be32(p + 4 * i);
    return v;
}

static void show_fdt(const u8 *fdt)
{
    color(HEAD);
    puts_("Device tree\n");
    if (be32(fdt) != 0xd00dfeed) {
        color(BAD);
        puts_("  bad magic: ");
        hexv(be32(fdt));
        puts_("\n");
        return;
    }
    u32 size = be32(fdt + 4), off_struct = be32(fdt + 8), off_str = be32(fdt + 12);
    kv("  size        ", size);

    const u8 *p = fdt + off_struct;
    const char *strs = (const char *)fdt + off_str;
    int depth = 0, acells = 2, scells = 1, in_mem = 0, in_chosen = 0, banks = 0;
    u64 total = 0;

    for (;;) {
        u32 tok = be32(p);
        p += 4;
        if (tok == 1) {                                 // BEGIN_NODE
            const char *name = (const char *)p;
            depth++;
            in_mem = depth == 2 && prefix(name, "memory");
            in_chosen = depth == 2 && streq(name, "chosen");
            int n = 0;
            while (name[n])
                n++;
            p += (n + 4) & ~3;
        } else if (tok == 2) {                          // END_NODE
            depth--;
            in_mem = in_chosen = 0;
        } else if (tok == 3) {                          // PROP
            u32 len = be32(p), nameoff = be32(p + 4);
            const u8 *val = p + 8;
            const char *pn = strs + nameoff;
            if (depth == 1 && streq(pn, "#address-cells"))
                acells = be32(val);
            if (depth == 1 && streq(pn, "#size-cells"))
                scells = be32(val);
            if (in_mem && streq(pn, "reg")) {
                for (u32 o = 0; o + 4 * (acells + scells) <= len; o += 4 * (acells + scells)) {
                    u64 base = cells(val + o, acells), sz = cells(val + o + 4 * acells, scells);
                    color(DIM);
                    puts_("  mem ");
                    color(FG);
                    hex(base, 9);
                    puts_(" + ");
                    hex(sz, 8);
                    color(DIM);
                    puts_("  ");
                    dec(sz >> 20);
                    puts_(" MiB\n");
                    total += sz;
                    banks++;
                }
            }
            if (in_chosen && (streq(pn, "linux,initrd-start") || streq(pn, "linux,initrd-end"))) {
                color(DIM);
                puts_(pn[13] == 's' ? "  initrd start " : "  initrd end   ");
                color(FG);
                hexv(cells(val, len / 4));
                putch('\n');
            }
            if (in_chosen && streq(pn, "bootargs")) {
                kv("  bootargs len ", len);
                for (u32 i = 0; i + 19 < len; i++)
                    if (prefix((const char *)val + i, "s3cfb.bootloaderfb=")) {
                        color(DIM);
                        puts_("  ");
                        color(FG);
                        for (u32 j = i; j < len && val[j] && val[j] != ' '; j++)
                            putch(val[j]);
                        putch('\n');
                    }
            }
            p += 8 + ((len + 3) & ~3);
        } else if (tok == 4) {                          // NOP
        } else {
            break;                                      // END or garbage
        }
    }
    color(DIM);
    puts_("  ");
    dec(banks);
    puts_(" banks, ");
    dec(total >> 20);
    puts_(" MiB total\n");
}

// ---------------------------------------------------------------- memory probe
#define BANK      0x880000000UL
#define BANK_SIZE 0x80000000UL

static void probe_bank(void)
{
    color(HEAD);
    puts_("Probe 2 GiB bank at 0x880000000\n");
    color(DIM);
    puts_("  sparse: 1 word per MiB ... ");

#ifdef HOST
    color(OK);
    puts_("2048/2048 OK\n");
    color(DIM);
    puts_("  dense: first 16 MiB ... ");
    color(OK);
    puts_("OK\n");
    return;
#endif
    // pass 1: write a pattern derived from the address
    for (u64 a = BANK; a < BANK + BANK_SIZE; a += 1 << 20)
        *(volatile u64 *)a = a ^ 0x5a5aa5a5c3c33c3cUL;
    // pass 2: read it all back
    u64 bad = 0, first_bad = 0;
    for (u64 a = BANK; a < BANK + BANK_SIZE; a += 1 << 20)
        if (*(volatile u64 *)a != (a ^ 0x5a5aa5a5c3c33c3cUL)) {
            if (!bad)
                first_bad = a;
            bad++;
        }
    if (bad) {
        color(BAD);
        dec(bad);
        puts_(" bad, first ");
        hexv(first_bad);
        putch('\n');
    } else {
        color(OK);
        puts_("2048/2048 OK\n");
    }

    color(DIM);
    puts_("  dense: first 16 MiB ... ");
    bad = 0;
    for (u64 a = BANK; a < BANK + (16 << 20); a += 8)
        *(volatile u64 *)a = ~a;
    for (u64 a = BANK; a < BANK + (16 << 20); a += 8)
        if (*(volatile u64 *)a != ~a)
            bad++;
    if (bad) {
        color(BAD);
        dec(bad);
        puts_(" bad words\n");
    } else {
        color(OK);
        puts_("OK\n");
    }
}

// ---------------------------------------------------------------- big counter
static void big_digit(int x0, int y0, int d, int px, u32 on, u32 off)
{
    // draw the font glyph for '0'+d at px pixels per font pixel
    const unsigned short *g = font['0' + d - 32];
    for (int y = 0; y < FONT_H; y++)
        for (int x = 0; x < FONT_W; x++)
            rect(x0 + x * px, y0 + y * px, px, px, (g[y] & (0x8000 >> x)) ? on : off);
}

static void counter(int y0)
{
    if (y0 > FB_H - 400)
        y0 = FB_H - 400;
    u64 f = SYSREG(cntfrq_el0);
    if (!f)
        f = 26000000;
    u64 t0 = SYSREG(cntpct_el0);
    const int px = 12;                                  // digit = 192 x 384 px
    for (u64 shown = ~0UL;;) {
        u64 s = (SYSREG(cntpct_el0) - t0) / f;
        if (s == shown)
            continue;
#ifdef HOST
        if (shown != ~0UL)
            return;
#endif
        shown = s;
        int x = (FB_W - 4 * FONT_W * px) / 2;
        u64 v = s % 10000;
        for (int i = 3; i >= 0; i--) {
            big_digit(x + i * FONT_W * px, y0, v % 10, px, OK, BG);
            v /= 10;
        }
    }
}


// ---------------------------------------------------------------- XNU loader
// Payload layout: see tools/mkpayload.py (shared with the Redmi shim).
#define BOOT_LINE_LENGTH 608
#define PAGE_16K 0x4000UL

struct payload_seg {
    u64 vmaddr, vmsize, file_off, file_size;
};

struct payload {
    char magic[8];
    u32 version, nsegs;
    u64 virt_base, entry_va, adt_off, adt_size, kernel_end;
    u64 phys_base, mem_size;
    char cmdline[BOOT_LINE_LENGTH];
    struct payload_seg segs[];
};

struct boot_video {
    u64 v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
    uint16_t Revision, Version;
    u64 virtBase, physBase, memSize, topOfKernelData;
    struct boot_video Video;
    u32 machineType;
    u64 deviceTreeP;
    u32 deviceTreeLength;
    char CommandLine[BOOT_LINE_LENGTH];
    u64 bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

#ifndef HOST
extern const u8 payload_start[];   // s9/shim/payload.S (empty without a payload)
#else
extern const u8 *host_payload;      // test_host.c
#define payload_start host_payload
#endif

// MMU off: every access is Device memory, so copy in aligned 8-byte words.
static void copy64(u64 dst, u64 src, u64 n)
{
    volatile u64 *d = (volatile u64 *)dst;
    volatile const u64 *s = (volatile const u64 *)src;
    for (u64 i = 0; i < (n + 7) / 8; i++)
        d[i] = s[i];
}

static void zero64(u64 dst, u64 n)
{
    volatile u64 *d = (volatile u64 *)dst;
    for (u64 i = 0; i < n / 8; i++)
        d[i] = 0;
}

// S-Boot leaves SCTLR_EL1.C set with the MMU off. Clean+invalidate the
// destination range by address (to the point of coherency) before copying, so
// no stale or dirty S-Boot line can shadow or be written back over what we
// copy for XNU. By-address maintenance works on physical addresses with the
// MMU off and, unlike set/way, is never trapped by a hypervisor.
static void dcache_civac_range(u64 start, u64 end)
{
#ifndef HOST
    u64 ctr = SYSREG(ctr_el0);
    u64 line = 4UL << ((ctr >> 16) & 0xf);      // DminLine, in bytes
    for (u64 a = start & ~(line - 1); a < end; a += line)
        __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy; isb" ::: "memory");
#else
    (void)start;
    (void)end;
#endif
}

static int payload_ok(const struct payload *p)
{
    const char want[8] = { 'S', 'W', 'E', 'E', 'T', 'X', 'N', 'U' };
    for (int i = 0; i < 8; i++)
        if (p->magic[i] != want[i])
            return 0;
    return 1;
}

typedef void (*xnu_entry_t)(u64 boot_args_pa);

// Returns only on failure (after printing why).
static void boot_xnu(void)
{
    const struct payload *p = (const struct payload *)payload_start;
    u64 src = (u64)payload_start;

    color(HEAD);
    puts_("XNU loader\n");
    if (!payload_ok(p)) {
        color(DIM);
        puts_("  no payload linked in: diagnostics\n");
        return;
    }
    if (p->version != 1 || (p->adt_off & 7) || (src & 7)) {
        color(BAD);
        puts_("  bad payload header\n");
        return;
    }
    u64 pa_of_va = p->phys_base - p->virt_base;     // pa = va + this
    kv("  payload     ", src);
    kv("  phys base   ", p->phys_base);
    kv("  mem size    ", p->mem_size);
    kv("  entry va    ", p->entry_va);
    kv("  segments    ", p->nsegs);

    if (p->phys_base != BANK || p->mem_size > BANK_SIZE) {
        color(BAD);
        puts_("  kernel RAM is not the 2 GiB bank\n");
        return;
    }

    // Everything we write: kernel segments, ADT, boot_args (one 16K page).
    u64 end = p->kernel_end + pa_of_va + p->adt_size + 2 * PAGE_16K;
    color(DIM);
    puts_("  cache flush ... ");
    dcache_civac_range(p->phys_base, end);
    color(OK);
    puts_("ok\n");

    color(DIM);
    puts_("  copy kernel ... ");
    for (u32 i = 0; i < p->nsegs; i++) {
        const struct payload_seg *sg = &p->segs[i];
        u64 dst = sg->vmaddr + pa_of_va;
        copy64(dst, src + sg->file_off, sg->file_size);
        u64 done = (sg->file_size + 7) & ~7UL;
        if (sg->vmsize > done)
            zero64(dst + done, sg->vmsize - done);
    }
    color(OK);
    puts_("ok\n");

    // ADT, then boot_args, right behind the kernel (below topOfKernelData).
    u64 adt_pa = p->kernel_end + pa_of_va;
    copy64(adt_pa, src + p->adt_off, p->adt_size);
    u64 ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
    u64 top = ba_pa + PAGE_16K;

    zero64(ba_pa, PAGE_16K);
    volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = p->virt_base;
    ba->physBase = p->phys_base;
    ba->memSize = p->mem_size;
    ba->topOfKernelData = top;
    ba->Video.v_baseAddr = 0xCC000000UL;
    ba->Video.v_display = 1;
    ba->Video.v_rowBytes = STRIDE * 4;
    ba->Video.v_width = FB_W;
    ba->Video.v_height = FB_H;
    ba->Video.v_depth = 32;
    ba->deviceTreeP = adt_pa - pa_of_va;            // XNU reads it with the MMU on: a VA
    ba->deviceTreeLength = (u32)p->adt_size;
    for (int i = 0; i < BOOT_LINE_LENGTH; i++)
        ba->CommandLine[i] = p->cmdline[i];
    ba->memSizeActual = p->mem_size;
    kv("  boot_args   ", ba_pa);
    kv("  ADT         ", adt_pa);

    u64 entry_pa = p->entry_va + pa_of_va;
    kv("  jump        ", entry_pa);
    color(OK);
    puts_("  jumping to XNU\n");
#ifndef HOST
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
    ((xnu_entry_t)entry_pa)(ba_pa);
#endif
    color(BAD);
    puts_("  XNU returned\n");
}


#ifndef HOST
// ---------------------------------------------------------------- GIC/timer test (t4)
// Which interrupts does the S9 actually deliver to non-secure EL1?
// GIC-400 through its memory-mapped registers (no GIC sysregs on this CPU).
#define GICD 0x10101000UL
#define GICC 0x10102000UL
#define TB_HZ 26000000UL                // CNTFRQ_EL0 reads 0 on the S9

static inline u32 rd32(u64 a) { return *(volatile u32 *)a; }
extern u32 safe_rd32(u64 a);
extern void safe_wr32(u64 a, u32 v);
extern volatile u32 safe_faults;
static inline void wr32(u64 a, u32 v) { *(volatile u32 *)a = v; }
#define WSR(name, v) __asm__ volatile("msr " #name ", %0" : : "r"((u64)(v)))

static volatile u32 irq_count[64], irq_spurious, irq_last = 0x3ff, irq_total;

void shim_irq(void)
{
    u32 iar = rd32(GICC + 0x0C);
    u32 id = iar & 0x3ff;
    irq_total++;
    if (id >= 1020) {
        irq_spurious++;
        return;
    }
    irq_last = id;
    if (id < 64)
        irq_count[id]++;
    if (id == 27)
        WSR(cntv_tval_el0, TB_HZ / 100);       // re-arm 10 ms
    else if (id == 30)
        WSR(cntp_tval_el0, TB_HZ / 100);
    else if (id == 26)
        WSR(cntv_ctl_el0, 0);
    wr32(GICC + 0x10, iar);
    __asm__ volatile("dsb sy; isb" ::: "memory");
}

static u64 now(void)
{
    __asm__ volatile("isb");
    return SYSREG(cntpct_el0);
}

static void wait_ticks(u64 t)
{
    u64 end = now() + t;
    while (now() < end)
        ;
}

static void kvh(const char *k, u64 a, const char *k2, u64 b)
{
    color(DIM);
    puts_(k);
    color(FG);
    hexv(a);
    color(DIM);
    puts_(k2);
    color(FG);
    hexv(b);
    putch('\n');
}

static void irq_window(const char *label, int src)
{
    for (int i = 0; i < 64; i++)
        irq_count[i] = 0;
    irq_spurious = 0;
    irq_total = 0;
    irq_last = 0x3ff;

    if (src == 27) {
        WSR(cntv_tval_el0, TB_HZ / 100);
        WSR(cntv_ctl_el0, 1);
    } else if (src == 30) {
        WSR(cntp_tval_el0, TB_HZ / 100);
        WSR(cntp_ctl_el0, 1);
    }
    __asm__ volatile("isb");
    wait_ticks(TB_HZ / 50);                 // 20 ms masked: timer must be pending now
    if (src == 1)
        wr32(GICD + 0xF00, ((rd32(GICD + 0x800) & 0xff) << 16) | 1);
    __asm__ volatile("dsb sy; isb");
    u64 ctl = src == 30 ? SYSREG(cntp_ctl_el0) : SYSREG(cntv_ctl_el0);
    u32 pend = rd32(GICD + 0x200);
    u32 hppir = rd32(GICC + 0x18);
    u64 isr = SYSREG(isr_el1);

    __asm__ volatile("msr daifclr, #2" ::: "memory");
    u64 end = now() + TB_HZ * 3 / 10;
    u64 next = now() + TB_HZ / 100;
    for (int sent = 1; now() < end;)
        if (src == 1 && sent < 10 && now() >= next) {
            wr32(GICD + 0xF00, ((rd32(GICD + 0x800) & 0xff) << 16) | 1);
            sent++;
            next += TB_HZ / 100;
        }
    __asm__ volatile("msr daifset, #2" ::: "memory");
    WSR(cntv_ctl_el0, 0);
    WSR(cntp_ctl_el0, 0);
    __asm__ volatile("isb");

    color(HEAD);
    puts_(label);
    putch('\n');
    color(DIM);
    puts_("  got ");
    color(irq_count[src] ? OK : BAD);
    dec(irq_count[src]);
    color(DIM);
    puts_(src == 1 ? " of 10" : " (want ~30)");
    puts_("  total ");
    color(FG);
    dec(irq_total);
    color(DIM);
    puts_(" spur ");
    color(FG);
    dec(irq_spurious);
    putch('\n');
    kvh("  pre: ctl ", ctl, "  pend ", pend);
    kvh("  pre: hppir ", hppir, "  isr ", isr);
    kvh("  last id ", irq_last, "", 0);
}

static void __attribute__((unused)) gic_test(void)
{
    color(HEAD);
    puts_("GIC-400 / timer test\n");
    kvh("  GICD ctlr ", rd32(GICD + 0x000), "  typer ", rd32(GICD + 0x004));
    kvh("  GICD iidr ", rd32(GICD + 0x008), "  itargets0 ", rd32(GICD + 0x800));
    kvh("  GICC ctlr ", rd32(GICC + 0x00), "  iidr ", rd32(GICC + 0xFC));
    kvh("  ISENABLER0 ", rd32(GICD + 0x100), "  ICFGR1 ", rd32(GICD + 0xC04));
    kvh("  CNTV-CNTP ", SYSREG(cntvct_el0) - SYSREG(cntpct_el0), "  cntkctl ", SYSREG(cntkctl_el1));

    // Same setup as the kernel (patch 0082), plus the physical timer.
    u32 want = (1U << 27) | (1U << 30) | (1U << 1);
    wr32(GICD + 0x180, ~want);
    wr32(GICD + 0x080, 0xFFFFFFFF);
    for (int i = 0; i < 32; i += 4)
        wr32(GICD + 0x400 + i, 0xA0A0A0A0);
    wr32(GICD + 0x100, want);
    wr32(GICC + 0x04, 0xF0);
    wr32(GICC + 0x08, 0);
    wr32(GICC + 0x00, 1);
    wr32(GICD + 0x000, rd32(GICD + 0x000) | 1);
    __asm__ volatile("dsb sy; isb");
    kvh("  after: GICD ctlr ", rd32(GICD), "  GICC ctlr ", rd32(GICC));
    kvh("  after: ISENABLER0 ", rd32(GICD + 0x100), "  pmr ", rd32(GICC + 0x04));

    irq_window("Virtual timer (INTID 27)", 27);
    irq_window("Physical timer (INTID 30)", 30);
    irq_window("SGI 1 to self", 1);
}
#else
void shim_irq(void) {}
#endif


#ifndef HOST
// ---------------------------------------------------------------- UFS probe (t5)
// Read-only: is the Exynos UFS host S-Boot used still running, and which
// descriptor format does it expect? Shows S-Boot's last request (slot 0 of
// its list), then sends TEST UNIT READY, READ CAPACITY and READ(10) of the
// GPT on LUN 0 in the same format. There is no SCSI WRITE in this code.
#define UFS_HCI 0x11120000UL                // standard UFSHCI registers
#define UFS_VS  0x11121100UL                // Exynos vendor-specific HCI
#define UFS_BLOCK 4096u

static inline u32 ufs_rd(u32 off) { return *(volatile u32 *)(UFS_HCI + off); }
static inline void ufs_wr(u32 off, u32 v) { *(volatile u32 *)(UFS_HCI + off) = v; }
static inline u32 vs_rd(u32 off) { return *(volatile u32 *)(UFS_VS + off); }
static inline void vs_wr(u32 off, u32 v) { *(volatile u32 *)(UFS_VS + off) = v; }
#define VS_UTRL_NEXUS_TYPE 0x40             // Exynos: bit n = slot n carries a SCSI command
static u32 ufs_is_before, ufs_is_after;     // interrupt status around the last command

struct utrd {
    u32 dw0, dw1, dw2, dw3, ucdba, ucdbau, resp, prdt;
};

// Command UPIU area, response UPIU area (1 KiB each, like S-Boot), PRDT
// (8 entries of 16 bytes = 32 KiB in 4 KiB pieces).
struct ucd {
    u8 cmd[1024];
    u8 resp[1024];
    u32 prdt[32];
};

// Probe #1 got OCS 0x07 (fatal error) with its buffers in the shim image
// (0x80xxxxxx). Keep everything the controller DMAs next to S-Boot's own
// request list (0xef000000-0xef002fff, free RAM: not in reserved-memory).
#define UFS_DMA 0xef100000UL
static struct utrd *const utrl = (struct utrd *)UFS_DMA;               // 32 x 32 B
#define ucd_buf (*(struct ucd *)(UFS_DMA + 0x1000))
static u8 *const ufs_data = (u8 *)(UFS_DMA + 0x10000);                 // 8 KiB
static u8 *const gpt_ents = (u8 *)(UFS_DMA + 0x20000);                 // 16 KiB
static u32 ufs_ct;                          // UTRD command type, copied from S-Boot
static int ufs_bytes;                       // offsets/lengths in bytes (Exynos quirk), else DWORDs
static u32 ufs_prdt_max = 4096;             // bytes per PRDT entry (Exynos HCI_TXPRDT_ENTRY_SIZE)

static void zero(void *p, u32 n)
{
    volatile u8 *b = p;
    for (u32 i = 0; i < n; i++)
        b[i] = 0;
}

static u32 rbe32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u64 rle64(const u8 *p) { u64 v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

// One SCSI command (read direction or no data) in slot 0. 0 = success.
static u32 ufs_lun;                         // UPIU LUN for the next ufs_scsi()
static int ufs_write_dir;                   // next ufs_scsi() sends data to the device

static u32 ufs_scsi(const u8 *cdb, u32 cdb_len, void *buf, u32 len, u32 *ocs, u32 *st)
{
    static u8 tag;
    zero(&utrl[0], sizeof(utrl[0]));
    zero(&ucd_buf, sizeof(struct ucd));
    ucd_buf.cmd[0] = 0x01;                  // COMMAND UPIU
    ucd_buf.cmd[1] = len ? (ufs_write_dir ? 0x20 : 0x40) : 0x00;   // W / R
    ucd_buf.cmd[2] = ufs_lun;               // 0 = LUN 0, 0xd0 = UFS device W-LUN
    ucd_buf.cmd[3] = ++tag;
    ucd_buf.cmd[12] = len >> 24;
    ucd_buf.cmd[13] = len >> 16;
    ucd_buf.cmd[14] = len >> 8;
    ucd_buf.cmd[15] = len;
    for (u32 i = 0; i < cdb_len; i++)
        ucd_buf.cmd[16 + i] = cdb[i];

    u64 ucd_pa = (u64)&ucd_buf;
    u32 unit = ufs_bytes ? 1 : 4;
    // data direction: 1 = memory to device, 2 = device to memory
    utrl[0].dw0 = (ufs_ct << 28) | ((len ? (ufs_write_dir ? 1u : 2u) : 0u) << 25) | (1u << 24);
    utrl[0].dw2 = 0x0f;
    utrl[0].ucdba = (u32)ucd_pa;
    utrl[0].ucdbau = (u32)(ucd_pa >> 32);
    utrl[0].resp = (1024u / unit) << 16 | (1024u / unit);
    if (len) {
        // Exynos: one PRDT entry covers at most 2^txprdt bytes (4 KiB here).
        u32 chunk = ufs_prdt_max, n = (len + chunk - 1) / chunk;
        if (n > 8)
            return 5;                       // more than our PRDT holds
        u64 b = (u64)buf;
        for (u32 i = 0; i < n; i++) {
            u32 piece = len - i * chunk < chunk ? len - i * chunk : chunk;
            ucd_buf.prdt[4 * i + 0] = (u32)(b + i * chunk);
            ucd_buf.prdt[4 * i + 1] = (u32)((b + i * chunk) >> 32);
            ucd_buf.prdt[4 * i + 3] = piece - 1;
        }
        utrl[0].prdt = (2048u / unit) << 16 | (ufs_bytes ? n * 16 : n);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    // Exynos (Linux ufs-exynos.c): mark slot 0 as a SCSI command (I_T_L_Q nexus),
    // and clear any completion/error status left by the previous request.
    vs_wr(VS_UTRL_NEXUS_TYPE, vs_rd(VS_UTRL_NEXUS_TYPE) | 1);
    ufs_is_before = ufs_rd(0x20);
    if (ufs_is_before)
        ufs_wr(0x20, ufs_is_before);
    __asm__ volatile("dsb sy" ::: "memory");
    ufs_wr(0x58, 1);                        // UTRLDBR slot 0
    u64 t0 = now();
    while (ufs_rd(0x58) & 1)
        if (now() - t0 > TB_HZ) {           // 1 s
            ufs_is_after = ufs_rd(0x20);
            ufs_wr(0x5c, ~1u);              // UTRLCLR slot 0
            return 1;
        }
    __asm__ volatile("dsb sy" ::: "memory");
    ufs_is_after = ufs_rd(0x20);
    *ocs = utrl[0].dw2 & 0xff;
    *st = ucd_buf.resp[7];
    // Exynos reports OCS 0x07 (FATAL_ERROR) for requests that did complete
    // (Linux: UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR): judge by the response UPIU.
    if (*ocs && *ocs != 0x07)
        return 2;
    if (ucd_buf.resp[0] != 0x21)
        return 3;
    return *st ? 4 : 0;
}

static u32 ufs_read(u32 lba, u32 count, void *buf)
{
    u8 cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
    u32 ocs, st;
    return ufs_scsi(cdb, 10, buf, count * UFS_BLOCK, &ocs, &st);
}


// ---- UniPro (UIC) commands. Link bring-up after S-Boot, following a friend's
// working Exynos 9810 sequence: the HCS bits say "ready" but the connected
// lane counts read 0 until DME_LINK_STARTUP + CPort setup.
#define UFS_UNIPRO 0x11110000UL
#define UIC_GET 0x01
#define UIC_SET 0x02
#define UIC_LINK_STARTUP 0x16
#define IS_UCCS (1u << 10)

// Returns the UIC result code (0 = success), 0xff on timeout; *val = ARG3.
static u32 uic_cmd(u32 cmd, u32 attr, u32 set_val, u32 *val)
{
    u64 t0 = now();
    while (!(ufs_rd(0x30) & (1u << 3)))     // HCS.UCRDY
        if (now() - t0 > TB_HZ)
            return 0xfe;
    ufs_wr(0x20, IS_UCCS);
    ufs_wr(0x94, attr << 16);               // ARG1: attribute ID, selector 0
    ufs_wr(0x98, 0);                        // ARG2: attr set type normal
    ufs_wr(0x9c, set_val);                  // ARG3
    __asm__ volatile("dsb sy" ::: "memory");
    ufs_wr(0x90, cmd);
    t0 = now();
    while (!(ufs_rd(0x20) & IS_UCCS))
        if (now() - t0 > TB_HZ / 2)
            return 0xff;
    ufs_wr(0x20, IS_UCCS);
    if (val)
        *val = ufs_rd(0x9c);
    return ufs_rd(0x98) & 0xff;
}

static u32 dme_get(u32 attr)
{
    u32 v = 0xdead;
    u32 r = uic_cmd(UIC_GET, attr, 0, &v);
    return r ? 0xe0000000 | r : v;
}

static void ufs_link_up(void)
{
    u32 tx = dme_get(0x1561), rx = dme_get(0x1581);   // PA_CONNECTED{TX,RX}DATALANES
    kvh("  lanes tx ", tx, "  rx ", rx);
    if (tx && tx < 0x10 && rx && rx < 0x10) {
        color(OK);
        puts_("  link already up\n");
        return;
    }
    ufs_wr(0x60, 0);                        // stop the transfer request list
    ufs_wr(0x80, 0);                        // and the task management list
    u32 r = uic_cmd(UIC_LINK_STARTUP, 0, 0, 0);
    u32 cnf = *(volatile u32 *)(UFS_UNIPRO + 0x7854);  // Exynos direct result
    kvh("  link startup ", r, "  cnf ", cnf);
    tx = dme_get(0x1561);
    rx = dme_get(0x1581);
    kvh("  lanes tx ", tx, "  rx ", rx);
    // CPort setup in Samsung's order: N_DEVICEID, N_DEVICEID_VALID,
    // T_PEERDEVICEID, T_CONNECTIONSTATE.
    static const u32 attrs[4][2] = { { 0x3000, 0 }, { 0x3001, 1 }, { 0x4021, 1 }, { 0x4020, 1 } };
    for (int i = 0; i < 4; i++)
        uic_cmd(UIC_SET, attrs[i][0], attrs[i][1], 0);
    kvh("  devid ", dme_get(0x3000) << 4 | dme_get(0x3001), "  peer/conn ",
        dme_get(0x4021) << 4 | dme_get(0x4020));
}

// NOP OUT -> NOP IN handshake in slot 0 (no data). 0 = NOP IN received.
static u32 ufs_nop(u32 *ocs)
{
    zero(&utrl[0], sizeof(utrl[0]));
    zero(&ucd_buf, sizeof(struct ucd));
    ucd_buf.cmd[0] = 0x00;                  // NOP OUT
    ucd_buf.cmd[3] = 0x7f;                  // tag
    u64 ucd_pa = (u64)&ucd_buf;
    u32 unit = ufs_bytes ? 1 : 4;
    utrl[0].dw0 = (ufs_ct << 28) | (1u << 24);
    utrl[0].dw2 = 0x0f;
    utrl[0].ucdba = (u32)ucd_pa;
    utrl[0].ucdbau = (u32)(ucd_pa >> 32);
    utrl[0].resp = (1024u / unit) << 16 | (1024u / unit);
    __asm__ volatile("dsb sy" ::: "memory");
    vs_wr(VS_UTRL_NEXUS_TYPE, vs_rd(VS_UTRL_NEXUS_TYPE) & ~1u);   // not a SCSI command
    u32 is = ufs_rd(0x20);
    if (is)
        ufs_wr(0x20, is);
    __asm__ volatile("dsb sy" ::: "memory");
    ufs_wr(0x58, 1);
    u64 t0 = now();
    while (ufs_rd(0x58) & 1)
        if (now() - t0 > TB_HZ) {
            *ocs = utrl[0].dw2 & 0xff;
            ufs_wr(0x5c, ~1u);
            return 1;
        }
    __asm__ volatile("dsb sy" ::: "memory");
    *ocs = utrl[0].dw2 & 0xff;
    return ucd_buf.resp[0] == 0x20 && ucd_buf.resp[3] == 0x7f ? 0 : 2;
}

// Response UPIU after CHECK CONDITION: header bytes 4-7 (response, status),
// then the data segment at byte 32: 2-byte sense length + fixed-format sense.
static void ufs_sense(void)
{
    const u8 *r = ucd_buf.resp;
    const u8 *sd = r + 34;
    u32 sl = (u32)r[32] << 8 | r[33];
    kvh("    resp ", rbe32(r + 4), "  sense len ", sl);
    kvh("    key ", sd[2] & 0xf, "  asc/ascq ", (u32)sd[12] << 8 | sd[13]);
}

static void __attribute__((unused)) ufs_probe(void)
{
    color(HEAD);
    puts_("UFS probe (read-only)\n");
    u32 cap = ufs_rd(0x00), hcs = ufs_rd(0x30), hce = ufs_rd(0x34);
    kvh("  ver ", ufs_rd(0x08), "  cap ", cap);
    kvh("  hce ", hce, "  hcs ", hcs);
    kvh("  is ", ufs_rd(0x20), "  ie ", ufs_rd(0x24));
    kvh("  utrlba ", ufs_rd(0x50), "  hi ", ufs_rd(0x54));
    kvh("  dbr ", ufs_rd(0x58), "  rsr ", ufs_rd(0x60));
    kvh("  vs txprdt ", vs_rd(0x00), "  rxprdt ", vs_rd(0x04));
    kvh("  vs 0x08 ", vs_rd(0x08), "  0x0c ", vs_rd(0x0c));
    kvh("  vs 0x10 ", vs_rd(0x10), "  0x30 ", vs_rd(0x30));

    // S-Boot's own last request: slot 0 of its list (read only).
    u64 sb_list = (u64)ufs_rd(0x50) | (u64)ufs_rd(0x54) << 32;
    u32 sb_resp = 0, sb_prdt = 0;
    if (sb_list && sb_list < 0x100000000UL) {
        const volatile struct utrd *d = (const volatile struct utrd *)sb_list;
        kvh("  sb dw0 ", d->dw0, "  dw2 ", d->dw2);
        kvh("  sb ucd ", d->ucdba, "  hi ", d->ucdbau);
        sb_resp = d->resp;
        sb_prdt = d->prdt;
        kvh("  sb resp ", sb_resp, "  prdt ", sb_prdt);
        u64 ucd = (u64)d->ucdba | (u64)d->ucdbau << 32;
        u32 poff = sb_prdt >> 16;
        if (ucd && ucd < 0x100000000UL && poff) {
            // PRDT offset as bytes and as DWORDs: one of them shows an address + count
            const volatile u32 *pb = (const volatile u32 *)(ucd + poff);
            const volatile u32 *pd = (const volatile u32 *)(ucd + poff * 4);
            kvh("  prdt@B ", pb[0], " ", pb[3]);
            kvh("  prdt@DW ", pd[0], " ", pd[3]);
            const volatile u8 *c = (const volatile u8 *)ucd;
            kvh("  sb cmd ", (u32)c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3],
                "  cdb ", (u32)c[16] << 24 | c[17] << 16 | c[18] << 8 | c[19]);
        }
    }
    // Byte granularity if S-Boot's response offset is 512+ (bytes) rather than 128 (DWORDs).
    ufs_bytes = (sb_resp >> 16) >= 512;
    if (sb_list && sb_list < 0x100000000UL)
        ufs_ct = ((const volatile struct utrd *)sb_list)->dw0 >> 28;
    u32 ent = vs_rd(0x00) & 0x1f;           // log2 of the bytes one PRDT entry may cover
    if (ent >= 9 && ent <= 17)
        ufs_prdt_max = 1u << ent;
    color(DIM);
    puts_("  format: ");
    color(FG);
    puts_(ufs_bytes ? "bytes" : "dwords");
    puts_(", max per PRDT entry ");
    dec(ufs_prdt_max);
    puts_(", CT ");
    dec(ufs_ct);
    putch('\n');
    kvh("  our list ", UFS_DMA, "  ucd ", UFS_DMA + 0x1000);

    // Exynos HCE reads 0 even while running (Linux: UFSHCI_QUIRK_BROKEN_HCE),
    // so only require device present + transfer list ready in HCS.
    (void)hce;
    if (!(hcs & 1) || !(hcs & 2)) {
        color(BAD);
        puts_("  host not enabled/ready: stop\n");
        return;
    }
    if (ufs_rd(0x58)) {
        color(BAD);
        puts_("  requests outstanding: stop\n");
        return;
    }
    ufs_link_up();
    // Take over the transfer request list; never ring a doorbell unless the
    // host really uses ours (else it would run S-Boot's slot 0 again).
    ufs_wr(0x60, 0);
    zero((void *)UFS_DMA, 0x30000);
    u64 list = (u64)utrl;
    ufs_wr(0x50, (u32)list);
    ufs_wr(0x54, (u32)(list >> 32));
    ufs_wr(0x60, 1);
    if (ufs_rd(0x50) != (u32)list || ufs_rd(0x54) != (u32)(list >> 32) || !(ufs_rd(0x60) & 1)) {
        color(BAD);
        puts_("  list base not taken: stop\n");
        return;
    }

    u32 ocs = 0, st = 0, rc = 1;
    for (int i = 0; i < 3 && rc; i++) {
        rc = ufs_nop(&ocs);
        kvh("  NOP rc ", rc, "  ocs ", ocs);
        kvh("    resp ", rbe32(ucd_buf.resp), "  dbr ", ufs_rd(0x58));
    }
    static const u8 tur[6] = { 0 };
    kvh("  nexus ", vs_rd(VS_UTRL_NEXUS_TYPE), "  is ", ufs_rd(0x20));

    // Probe #6: LUN 0 answers NOT READY, ASC/ASCQ 04/02 "initializing command
    // required": S-Boot left the UFS device powered down. Wake it like Linux's
    // ufshcd_set_dev_pwr_mode(): START STOP UNIT, power condition ACTIVE (1),
    // to the UFS device well-known LUN 0xd0. A power command; no data written.
    static const u8 ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
    ufs_lun = 0xd0;
    rc = 1;
    for (int i = 0; i < 4 && rc; i++) {
        rc = ufs_scsi(ssu, 6, 0, 0, &ocs, &st);
        kvh("  SSU rc ", rc, "  ocs/st ", ocs << 8 | st);
        if (rc)
            ufs_sense();
    }
    ufs_lun = 0;
    rc = 1;
    for (int i = 0; i < 6 && rc; i++) {     // unit attentions first (power on, reset, ...)
        rc = ufs_scsi(tur, 6, 0, 0, &ocs, &st);
        kvh("  TUR rc ", rc, "  ocs/st ", ocs << 8 | st);
        if (rc)
            ufs_sense();
    }
    static const u8 cap10[10] = { 0x25 };
    rc = ufs_scsi(cap10, 10, ufs_data, 8, &ocs, &st);
    kvh("  CAP rc ", rc, "  ocs/st ", ocs << 8 | st);
    if (rc)
        ufs_sense();
    if (!rc)
        kvh("  LUN0 last lba ", rbe32(ufs_data), "  block ", rbe32(ufs_data + 4));

    static const u8 rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };   // LBA 0, 2 blocks
    rc = ufs_scsi(rd10, 10, ufs_data, 2 * UFS_BLOCK, &ocs, &st);
    if (rc)
        ufs_sense();
    kvh("  READ rc ", rc, "  ocs/st ", ocs << 8 | st);
    if (rc)
        return;
    const u8 *g = ufs_data + UFS_BLOCK;
    color(DIM);
    puts_("  GPT sig ");
    color(g[0] == 'E' && g[1] == 'F' && g[2] == 'I' ? OK : BAD);
    for (int i = 0; i < 8; i++)
        putch(g[i] >= 32 && g[i] < 127 ? g[i] : '.');
    putch('\n');
    u32 nent = g[80] | g[81] << 8, esz = g[84] | g[85] << 8, elba = g[72] | g[73] << 8;
    kvh("  entries ", nent, "  at lba ", elba);
    if (esz != 128 || nent > 128 || ufs_read(elba, (nent * 128 + UFS_BLOCK - 1) / UFS_BLOCK, gpt_ents))
        return;
    // The partitions that matter later, with their 4K block ranges.
    u64 ud_first = 0, ud_last = 0;
    for (u32 i = 0; i < nent; i++) {
        const u8 *e = gpt_ents + i * 128;
        u64 first = rle64(e + 32), last = rle64(e + 40);
        if (!first)
            continue;
        char name[13];
        for (int c = 0; c < 12; c++)
            name[c] = e[56 + 2 * c];
        name[12] = 0;
        if (!(name[0] == 'U' && name[1] == 'S') && !(name[0] == 'B' && name[1] == 'O') &&
            !(name[0] == 'S' && name[1] == 'Y'))
            continue;                       // USERDATA, BOOT, SYSTEM only
        if (name[0] == 'U' && name[1] == 'S' && name[2] == 'E' && name[3] == 'R') {
            ud_first = first;
            ud_last = last;
        }
        color(FG);
        puts_("  ");
        puts_(name);
        color(DIM);
        puts_(" lba ");
        color(FG);
        hexv(first);
        color(DIM);
        puts_(" MiB ");
        color(FG);
        dec((last - first + 1) >> 8);
        putch('\n');
    }

    // Probe #8: WRITE test. XNU's persistent log never reached USERDATA, so
    // check the write path alone: 2 blocks (2 PRDT entries) at the very end of
    // USERDATA, the kernel log area, far behind the 16 GiB iOS volume at its
    // start. Write a pattern, SYNCHRONIZE CACHE, read back into another buffer.
    color(HEAD);
    puts_("UFS write test (end of USERDATA)\n");
    if (!ud_last || ud_last - ud_first < 0x100000) {
        color(BAD);
        puts_("  USERDATA not found: skip\n");
        return;
    }
    u32 wlba = (u32)(ud_last - 1);
    u8 *wbuf = ufs_data, *rbuf = gpt_ents;
    static u32 seq;
    seq++;
    for (u32 i = 0; i < 2 * UFS_BLOCK; i++)
        wbuf[i] = (u8)(i * 7 + seq + (i >> 12));
    const char tag[] = "S9WRTEST";
    for (int i = 0; i < 8; i++)
        wbuf[i] = tag[i];
    kvh("  lba ", wlba, "  of last ", (u32)ud_last);
    u8 wr10[10] = { 0x2a, 0, wlba >> 24, wlba >> 16, wlba >> 8, wlba, 0, 0, 2, 0 };
    ufs_write_dir = 1;
    rc = ufs_scsi(wr10, 10, wbuf, 2 * UFS_BLOCK, &ocs, &st);
    ufs_write_dir = 0;
    kvh("  WRITE rc ", rc, "  ocs/st ", ocs << 8 | st);
    if (rc)
        ufs_sense();
    static const u8 sync10[10] = { 0x35 };
    u32 rc2 = ufs_scsi(sync10, 10, 0, 0, &ocs, &st);
    kvh("  SYNC rc ", rc2, "  ocs/st ", ocs << 8 | st);
    zero(rbuf, 2 * UFS_BLOCK);
    u8 rd2[10] = { 0x28, 0, wlba >> 24, wlba >> 16, wlba >> 8, wlba, 0, 0, 2, 0 };
    u32 rc3 = ufs_scsi(rd2, 10, rbuf, 2 * UFS_BLOCK, &ocs, &st);
    u32 bad = 0, first_bad = 0xffffffff;
    for (u32 i = 0; i < 2 * UFS_BLOCK; i++)
        if (rbuf[i] != wbuf[i]) {
            if (first_bad == 0xffffffff)
                first_bad = i;
            bad++;
        }
    kvh("  READBACK rc ", rc3, "  bad bytes ", bad);
    if (bad)
        kvh("  first bad at ", first_bad, "  got/want ", (u32)rbuf[first_bad] << 8 | wbuf[first_bad]);
    color(!rc && !rc3 && !bad ? OK : BAD);
    puts_(!rc && !rc3 && !bad ? "  WRITE PATH OK\n" : "  WRITE PATH BROKEN\n");
}
#endif


// ---------------------------------------------------------------- input probe (t6)
// Buttons and touchscreen of the S9, from the stock device tree:
//   keys    pinctrl 0x14050000 (alive): vol+ gpa0-3, vol- gpa0-4, bixby gpa0-6,
//           power gpa2-4 (gpio-keys, active low)
//   touch   S6SY761 ("sec,sec_ts") at I2C 0x48 on hsi2c10 0x104B0000 (USI v2),
//           SCL/SDA gpp1-4/5 (pinctrl 0x10430000, function 2), attn gpa1-0
// Read-only except: I2C pins to their I2C function, the HSI2C block's own
// registers, and an I2C read of the touch chip's device ID (command 0x52).
#ifdef INPUTTEST
#define PIN_ALIVE  0x14050000UL
#define PIN_PERIC0 0x10430000UL
#define HSI2C10    0x104B0000UL
#define USI_SWCONF 0x10411000UL

static u32 bank_dat(u64 base, u32 bank) { return rd32(base + bank * 0x20 + 4); }
// Probe #4: the alive banks are one slot further than assumed: gpa0 at +0x20
// (DAT 0x5e: vol+/vol-/bixby high = released), gpa1 at +0x40, gpa2 at +0x60
// (power gpa2-4 went 0x14 -> 0x04 on a press).
#define GPA0 1
#define GPA1 2
#define GPA2 3

static void __attribute__((unused)) i2c_dump(void)
{
    static const u32 r[] = { 0x00, 0x04, 0x20, 0x24, 0x30, 0x40, 0x44, 0x50, 0x60, 0x64, 0x68, 0x6c, 0x70, 0xc0, 0xc4, 0xc8, 0xcc };
    for (u32 i = 0; i < sizeof(r) / sizeof(r[0]); i += 2) {
        color(DIM);
        puts_("  +");
        hex(r[i], 2);
        puts_(" ");
        color(FG);
        hexv(rd32(HSI2C10 + r[i]));
        if (i + 1 < sizeof(r) / sizeof(r[0])) {
            color(DIM);
            puts_("  +");
            hex(r[i + 1], 2);
            puts_(" ");
            color(FG);
            hexv(rd32(HSI2C10 + r[i + 1]));
        }
        putch('\n');
    }
}

// HSI2C (Linux i2c-exynos5.c, exynos7 variant) in auto mode, polled.
#define HS_CTL 0x00
#define HS_FIFO_CTL 0x04
#define HS_INT_EN 0x20
#define HS_INT_STAT 0x24
#define HS_FIFO_STAT 0x30
#define HS_TX 0x34
#define HS_RX 0x38
#define HS_CONF 0x40
#define HS_AUTO_CONF 0x44
#define HS_TIMEOUT 0x48
#define HS_TRANS_STAT 0x50
#define HS_FS1 0x60
#define HS_FS2 0x64
#define HS_FS3 0x68
#define HS_SLA 0x6c
#define HS_ADDR 0x70

static void hs_init(void)
{
    u32 ctl = rd32(HSI2C10 + HS_CTL);
    wr32(HSI2C10 + HS_CTL, ctl | (1u << 31));           // SW_RST
    wait_ticks(TB_HZ / 10000);
    wr32(HSI2C10 + HS_CTL, ctl & ~(1u << 31));
    // 100 kHz from a 200 MHz ipclk (default-clk in the device tree)
    u32 ftl = (rd32(HSI2C10 + HS_CONF) >> 16) & 7;
    u32 temp = 200000000u / 100000u - 8 - ftl, div = 0;
    while (temp / (div + 1) > 510)
        div++;
    u32 cyc = temp / (div + 1) - 2, l = cyc / 2;
    if (l > 255)
        l = 255;
    wr32(HSI2C10 + HS_FS1, l << 24 | l << 16 | l << 8);
    wr32(HSI2C10 + HS_FS2, (l / 2) << 24 | l << 8 | l);
    wr32(HSI2C10 + HS_FS3, div << 16 | cyc);
    wr32(HSI2C10 + HS_SLA, l / 2);
    wr32(HSI2C10 + HS_CONF, 1u << 31);                    // AUTO_MODE
    wr32(HSI2C10 + HS_CTL, 1u << 0 | 1u << 3);            // I2C function, master
    wr32(HSI2C10 + HS_FIFO_CTL, 1u << 0 | 1u << 1 | 1u << 4 | 1u << 16);   // RX/TX FIFO on
    wr32(HSI2C10 + HS_TIMEOUT, 1u << 31 | 0xff);
    wr32(HSI2C10 + HS_INT_EN, 0xff80);                    // done/abort/no ack/no dev/timeout
    wr32(HSI2C10 + HS_ADDR, 0x48u << 10);
}

// One auto-mode message to address 0x48 with STOP. 0 = done; else status bits.
static u32 __attribute__((unused)) hs_xfer(int read, u8 *buf, u32 len)
{
    wr32(HSI2C10 + HS_INT_STAT, rd32(HSI2C10 + HS_INT_STAT));
    u32 ctl = rd32(HSI2C10 + HS_CTL) & ~(3u << 6);
    wr32(HSI2C10 + HS_CTL, ctl | (read ? 1u << 6 : 1u << 7));   // RXCHON / TXCHON
    u32 auto_conf = len | 1u << 17 | (read ? 1u << 16 : 0);     // STOP_AFTER_TRANS, READ
    wr32(HSI2C10 + HS_AUTO_CONF, auto_conf);
    if (!read)
        for (u32 i = 0; i < len; i++)
            wr32(HSI2C10 + HS_TX, buf[i]);
    wr32(HSI2C10 + HS_AUTO_CONF, auto_conf | 1u << 31);         // MASTER_RUN
    u64 t0 = now();
    u32 got = 0, st = 0;
    while (now() - t0 < TB_HZ / 5) {                             // 200 ms
        if (read)
            while (got < len && !(rd32(HSI2C10 + HS_FIFO_STAT) & (1u << 24)))
                buf[got++] = (u8)rd32(HSI2C10 + HS_RX);
        st = rd32(HSI2C10 + HS_INT_STAT);
        if (st & 0xff80)
            break;
    }
    if (read)
        while (got < len && !(rd32(HSI2C10 + HS_FIFO_STAT) & (1u << 24)))
            buf[got++] = (u8)rd32(HSI2C10 + HS_RX);
    kvh("    int ", st, "  trans ", rd32(HSI2C10 + HS_TRANS_STAT));
    if (read && got < len)
        return 0x80000000 | got;
    return (st & 0x7f00) ? st : (st & 0x80 ? 0 : 0x40000000 | st);
}


// SPEEDY (Samsung one-wire PMIC bus) at 0x141C0000, polled. Register map and
// sequence from Markuss Broks' Linux driver for Exynos9810 (LKML 2024-12,
// "soc: samsung: Add a driver for Samsung SPEEDY host controller").
// Address = (device << 8 | register) << 7; S2MPS18 PMIC block = device 1.
#define SPEEDY 0x141C0000UL
#define SP_CTRL 0x000
#define SP_FIFO_CTRL 0x004
#define SP_CMD 0x008
#define SP_INT_EN 0x00c
#define SP_INT_ST 0x010
#define SP_TX 0x034
#define SP_RX 0x038

static void sp_prep(void)
{
    safe_wr32(SPEEDY + SP_FIFO_CTRL, safe_rd32(SPEEDY + SP_FIFO_CTRL) | 1u << 31);   // FIFO reset
    wait_ticks(TB_HZ / 100000);
    safe_wr32(SPEEDY + SP_FIFO_CTRL, safe_rd32(SPEEDY + SP_FIFO_CTRL) | 1u | 1u << 8);  // RX/TX length 1
    safe_wr32(SPEEDY + SP_INT_ST, 0xffffffffu);
    wait_ticks(TB_HZ / 100000);
}

static int sp_wait(void)
{
    u64 t0 = now();
    while (now() - t0 < TB_HZ / 20)                              // 50 ms
        if (safe_rd32(SPEEDY + SP_INT_ST) & 1)
            return 0;
    return -1;
}

// 0..255 = value, < 0 = error
static int speedy_read(u32 dev, u32 reg)
{
    sp_prep();
    safe_wr32(SPEEDY + SP_INT_EN, 1u | 1u << 8 | 1u << 9 | 7u << 16 | 1u << 31);
    safe_wr32(SPEEDY + SP_CMD, 1u << 19 | ((dev & 0xf) << 15) | ((reg & 0xff) << 7));
    if (sp_wait())
        return -1;
    u32 v = safe_rd32(SPEEDY + SP_RX);
    safe_wr32(SPEEDY + SP_INT_ST, 0xffffffffu);
    return (int)(v & 0xff);
}

static int speedy_write(u32 dev, u32 reg, u32 val)
{
    sp_prep();
    safe_wr32(SPEEDY + SP_INT_EN, 1u | 1u << 4 | 1u << 20 | 1u << 21 | 1u << 31);
    safe_wr32(SPEEDY + SP_CMD, 1u << 19 | 1u << 20 | ((dev & 0xf) << 15) | ((reg & 0xff) << 7));
    safe_wr32(SPEEDY + SP_TX, val & 0xff);
    int r = sp_wait();
    safe_wr32(SPEEDY + SP_INT_ST, 0xffffffffu);
    return r;
}

// Touch power: S2MPS18 LDO35 (tsp_io 1.8 V, reg 0x6C) and LDO43 (tsp_avdd
// 3.3 V, reg 0x74); enable = bits 7:6 (Samsung s2mps18-private.h). Read,
// show, then only set the enable bits, voltage bits untouched.
static void touch_power(void)
{
    color(HEAD);
    puts_("Touch power (S2MPS18 via SPEEDY)\n");
    safe_faults = 0;
    u32 ctrl = safe_rd32(SPEEDY + SP_CTRL);
    kvh("  speedy ctrl ", ctrl, "  faults ", safe_faults);
    if (safe_faults)
        return;
    if (!(ctrl & 1)) {                                          // not enabled: init like Linux
        safe_wr32(SPEEDY + SP_INT_ST, 0xffffffffu);
        safe_wr32(SPEEDY + SP_CTRL, ctrl | 1u << 31);
        wait_ticks(TB_HZ / 50);
        safe_wr32(SPEEDY + SP_CTRL, safe_rd32(SPEEDY + SP_CTRL) | 1u);
        wait_ticks(TB_HZ / 50);
    }
    int l35 = speedy_read(1, 0x6c), l43 = speedy_read(1, 0x74), id = speedy_read(0, 0x00);
    kvh("  L35 ", (u32)l35, "  L43 ", (u32)l43);
    kvh("  top 0x00 ", (u32)id, "", 0);
    if (l35 < 0 || l43 < 0 || (l35 & 0x3f) == 0 || (l43 & 0x3f) == 0) {
        color(BAD);
        puts_("  read failed or no voltage set: not touching the PMIC\n");
        return;
    }
    int w1 = (l35 & 0xc0) == 0xc0 ? 0 : speedy_write(1, 0x6c, (u32)l35 | 0xc0);
    int w2 = (l43 & 0xc0) == 0xc0 ? 0 : speedy_write(1, 0x74, (u32)l43 | 0xc0);
    wait_ticks(TB_HZ / 10);                                      // ramp + touch boot
    kvh("  after L35 ", (u32)speedy_read(1, 0x6c), "  L43 ", (u32)speedy_read(1, 0x74));
    kvh("  write rc ", (u32)(w1 & 0xff) << 8 | (u32)(w2 & 0xff), "", 0);
    kvh("  gpp1 dat ", bank_dat(PIN_PERIC0, 1), "  gpa1 dat ", bank_dat(PIN_ALIVE, GPA1));
    wait_ticks(TB_HZ / 5);                                       // S6SY761 needs ~100 ms after power
}


// One auto-mode message to addr with STOP, completion from TRANS_STATUS
// (Samsung i2c-exynos5.c for USI v2: bit0 TRANS_DONE, bit1 ABORT, bit2
// NO_DEV_ACK, bit3 NO_DEV, bit4 TIMEOUT_AUTO, bit17 MASTER_BUSY).
// Returns TRANS_STATUS; bytes read in *got.
static u32 hs_msg(u32 addr, int read, u8 *buf, u32 len, u32 *got)
{
    u32 ctl = rd32(HSI2C10 + HS_CTL) & ~(3u << 6);
    wr32(HSI2C10 + HS_FIFO_CTL, 1u | 2u | (len << 4) | (len << 16));
    wr32(HSI2C10 + HS_ADDR, (7u << 24) | ((addr & 0x7f) << 10));
    wr32(HSI2C10 + HS_CTL, ctl | (read ? 1u << 6 : 1u << 7));
    wr32(HSI2C10 + HS_INT_EN, 1u << 7);
    wr32(HSI2C10 + HS_INT_STAT, 0xffffffffu);
    u32 ac = len | 1u << 17 | (read ? 1u << 16 : 0);
    wr32(HSI2C10 + HS_AUTO_CONF, ac);
    if (!read)
        for (u32 i = 0; i < len; i++)
            wr32(HSI2C10 + HS_TX, buf[i]);
    wr32(HSI2C10 + HS_AUTO_CONF, ac | 1u << 31);
    u64 t0 = now();
    u32 ts = 0, n = 0;
    while (now() - t0 < TB_HZ / 50) {                            // 20 ms
        if (read)
            while (n < len && !(rd32(HSI2C10 + HS_FIFO_STAT) & (1u << 24)))
                buf[n++] = (u8)rd32(HSI2C10 + HS_RX);
        ts = rd32(HSI2C10 + HS_TRANS_STAT);
        if (ts & 0x1f)
            break;
    }
    if (read)
        while (n < len && !(rd32(HSI2C10 + HS_FIFO_STAT) & (1u << 24)))
            buf[n++] = (u8)rd32(HSI2C10 + HS_RX);
    if (got)
        *got = n;
    if (ts & 0x1e) {                                             // error: reset the block for the next one
        u32 c = rd32(HSI2C10 + HS_CTL);
        wr32(HSI2C10 + HS_CTL, c | 1u << 31);
        wr32(HSI2C10 + HS_CTL, c & ~(1u << 31));
        wr32(HSI2C10 + HS_CTL, 1u << 3);
        wr32(HSI2C10 + HS_CONF, rd32(HSI2C10 + HS_CONF) | 1u << 31);
    }
    return ts;
}


// Samsung i2c-exynos5.c (USI v2) init: SW reset, CTL = MASTER only, trailing
// count, CONF |= AUTO_MODE, TIMEOUT_EN off; timing like
// exynos5_i2c_set_timing(): only FS3[23:16] div, FS2[7:0] and FS1[23:16].
// mode 0: reset defaults, 1/2: fast 400 kHz with ipclk 200 / 26 MHz,
// 3: standard 100 kHz with ipclk 200 MHz.
static void hs_samsung(int mode)
{
    u32 c = rd32(HSI2C10 + HS_CTL);
    wr32(HSI2C10 + HS_CTL, c | 1u << 31);
    wr32(HSI2C10 + HS_CTL, c & ~(1u << 31));
    if (mode) {
        u32 ip = mode == 2 ? 26000000u : 200000000u;
        u32 op = mode == 3 ? 100000u : 400000u;
        u32 mul = mode == 3 ? 25 : 9;
        u32 div = (ip / (op * (mode == 3 ? 16 : 15))) & 0xff;
        wr32(HSI2C10 + HS_FS3, (rd32(HSI2C10 + HS_FS3) & ~0xff0000u) | div << 16);
        u32 h = (mul * (ip / 1000000)) / ((div + 1) * 10);
        u32 hd = h ? h - 1 : 0;
        u32 vh = mode == 3 ? (0xffu << h) & 0xff : ((0xffffffffu >> h) << h) & 0xff;
        u32 vs = mode == 3 ? (0xffu << hd) & 0xff : ((0xffffffffu >> hd) << hd) & 0xff;
        if (h >= 32) vh = 0;
        if (hd >= 32) vs = 0;
        wr32(HSI2C10 + HS_FS2, (rd32(HSI2C10 + HS_FS2) & ~0xffu) | vh);
        wr32(HSI2C10 + HS_FS1, (rd32(HSI2C10 + HS_FS1) & ~0xff0000u) | vs << 16);
    }
    wr32(HSI2C10 + HS_CTL, 1u << 3);                                    // MASTER
    wr32(HSI2C10 + 0x08, 0xffffff);                                     // TRAILIG_CTL
    wr32(HSI2C10 + HS_CONF, rd32(HSI2C10 + HS_CONF) | 1u << 31);        // AUTO_MODE
    wr32(HSI2C10 + HS_TIMEOUT, rd32(HSI2C10 + HS_TIMEOUT) & ~(1u << 31));
}


// Does the HSI2C take a timing write? (FS3 div field; needs the IP clock)
static int hs_timing_sticks(void)
{
    u32 c = rd32(HSI2C10 + HS_CTL);
    wr32(HSI2C10 + HS_CTL, c | 1u << 31);
    wr32(HSI2C10 + HS_CTL, c & ~(1u << 31));
    u32 old = rd32(HSI2C10 + HS_FS3);
    u32 want = (old & ~0xff0000u) | (0x21u << 16);
    wr32(HSI2C10 + HS_FS3, want);
    u32 got = rd32(HSI2C10 + HS_FS3);
    wr32(HSI2C10 + HS_FS3, old);
    return got == want;
}

// Gate-shaped registers (bits 20/21/28/29 only) in [lo, hi) of a CMU: manual
// mode + clock on, like Linux exynos_arm64_init_clocks + clk_enable.
static u32 cmu_gates_on(u64 cmu, u32 lo, u32 hi)
{
    u32 on = 0;
    for (u32 o = lo; o < hi; o += 4) {
        safe_faults = 0;
        u32 v = safe_rd32(cmu + o);
        if (safe_faults || (v & ~0x30300000u))
            continue;
        if (!(v & (1u << 21)) || (v & (1u << 28))) {
            safe_wr32(cmu + o, (v & ~(1u << 28)) | 1u << 20 | 1u << 21);
            on++;
        }
    }
    return on;
}

static void ipclk_fix(void)
{
    color(HEAD);
    puts_("HSI2C IP clock\n");
    kvh("  timing sticks ", hs_timing_sticks(), "", 0);
    if (hs_timing_sticks())
        return;
    // A: CMU_TOP (0x15a80000, as Exynos 8895) gates incl. CLKCMU_PERIC0_*
    u32 n = cmu_gates_on(0x15a80000UL, 0x2000, 0x2400);
    kvh("  A top gates on ", n, "  sticks ", hs_timing_sticks());
    if (hs_timing_sticks())
        return;
    // B: CMU_PERIC0 user muxes (PLL_CON0 .._USER at 0x100 + 0x20 i, bit 4 =
    // CMU clock, 0 = OSCCLK): every USI mux (i >= 2: not BUS, not UART_DBG)
    // to the 26 MHz oscillator, which always runs.
    color(DIM);
    puts_("  B mux ");
    color(FG);
    for (u32 i = 2; i < 16; i++) {
        u64 a = 0x10400000UL + 0x100 + 0x20 * i;
        safe_faults = 0;
        u32 v = safe_rd32(a);
        if (safe_faults)
            continue;
        hex(i, 1);
        putch(':');
        hex(v & 0xff, 2);
        putch(' ');
        if (v & (1u << 4))
            safe_wr32(a, v & ~(1u << 4));
    }
    putch('\n');
    kvh("  B sticks ", hs_timing_sticks(), "", 0);
}


// Bit-banged I2C on gpp1-4 (SCL) / gpp1-5 (SDA), open drain emulated:
// high = input (released, external pull-up), low = output driving 0.
#define BB_CON (PIN_PERIC0 + 0x20)
#define BB_DAT (PIN_PERIC0 + 0x24)
#define BB_PUD (PIN_PERIC0 + 0x28)
#define BB_SCL 4
#define BB_SDA 5

static void bb_line(u32 pin, int high)
{
    u32 con = rd32(BB_CON) & ~(0xfu << (pin * 4));
    if (!high) {
        wr32(BB_DAT, rd32(BB_DAT) & ~(1u << pin));
        con |= 1u << (pin * 4);                                  // output, 0
    }
    wr32(BB_CON, con);
}

static int bb_get(u32 pin) { return (rd32(BB_DAT) >> pin) & 1; }
static void bb_delay(void) { wait_ticks(TB_HZ / 200000); }       // 5 us: ~100 kHz

static int bb_scl_high(void)                                     // with clock stretching
{
    bb_line(BB_SCL, 1);
    u64 t0 = now();
    while (!bb_get(BB_SCL))
        if (now() - t0 > TB_HZ / 100)                            // 10 ms
            return -1;
    return 0;
}

static void bb_start(void)
{
    bb_line(BB_SDA, 1);
    bb_scl_high();
    bb_delay();
    bb_line(BB_SDA, 0);
    bb_delay();
    bb_line(BB_SCL, 0);
    bb_delay();
}

static void bb_stop(void)
{
    bb_line(BB_SDA, 0);
    bb_delay();
    bb_scl_high();
    bb_delay();
    bb_line(BB_SDA, 1);
    bb_delay();
}

// returns 0 = ACK, 1 = NACK, -1 = SCL stuck low
static int bb_write(u32 byte)
{
    for (int i = 7; i >= 0; i--) {
        bb_line(BB_SDA, (byte >> i) & 1);
        bb_delay();
        if (bb_scl_high())
            return -1;
        bb_delay();
        bb_line(BB_SCL, 0);
    }
    bb_line(BB_SDA, 1);
    bb_delay();
    if (bb_scl_high())
        return -1;
    int nack = bb_get(BB_SDA);
    bb_delay();
    bb_line(BB_SCL, 0);
    bb_delay();
    return nack;
}

static u32 bb_read(int ack)
{
    u32 v = 0;
    bb_line(BB_SDA, 1);
    for (int i = 0; i < 8; i++) {
        bb_delay();
        bb_scl_high();
        v = v << 1 | (u32)bb_get(BB_SDA);
        bb_delay();
        bb_line(BB_SCL, 0);
    }
    bb_line(BB_SDA, !ack);
    bb_delay();
    bb_scl_high();
    bb_delay();
    bb_line(BB_SCL, 0);
    bb_line(BB_SDA, 1);
    return v;
}

static void bb_test(void)
{
    color(HEAD);
    puts_("Bit-banged I2C on gpp1-4/5\n");
    wr32(BB_PUD, rd32(BB_PUD) & ~(0xfu << (BB_SCL * 4) | 0xfu << (BB_SDA * 4)));  // no pulls
    bb_line(BB_SCL, 1);
    bb_line(BB_SDA, 1);
    wait_ticks(TB_HZ / 1000);
    kvh("  idle scl ", bb_get(BB_SCL), "  sda ", bb_get(BB_SDA));
    // 9 clocks to free a stuck slave, then STOP
    for (int i = 0; i < 9; i++) {
        bb_line(BB_SCL, 0);
        bb_delay();
        bb_scl_high();
        bb_delay();
    }
    bb_stop();
    color(DIM);
    puts_("  ack: ");
    color(OK);
    u32 n = 0;
    for (u32 a = 0x08; a < 0x78; a++) {
        bb_start();
        int r = bb_write(a << 1 | 1);
        if (r == 0) {
            (void)bb_read(0);
            hexv(a);
            putch(' ');
            n++;
        }
        bb_stop();
        if (r < 0) {
            color(BAD);
            puts_("SCL stuck ");
            break;
        }
    }
    color(FG);
    putch('\n');
    kvh("  devices ", n, "", 0);
    // S6SY761 device ID: write 0x52, read 3
    bb_start();
    int a1 = bb_write(0x48 << 1), a2 = bb_write(0x52);
    bb_stop();
    bb_start();
    int a3 = bb_write(0x48 << 1 | 1);
    u32 id = 0;
    if (a3 == 0) {
        id = bb_read(1) << 16;
        id |= bb_read(1) << 8;
        id |= bb_read(0);
    }
    bb_stop();
    kvh("  0x48 acks ", (u32)(a1 & 0xf) << 8 | (u32)(a2 & 0xf) << 4 | (u32)(a3 & 0xf), "  id ", id);
    color(!a1 && !a2 && !a3 ? OK : BAD);
    puts_(!a1 && !a2 && !a3 ? "  TOUCH CHIP ANSWERS (bit-bang)\n" : "  no answer (bit-bang)\n");
    color(FG);
}

static void input_test(void)
{
    color(HEAD);
    puts_("Input probe (t6)\n");
    kvh("  gpa0 con ", rd32(PIN_ALIVE + 0x00), "  dat ", bank_dat(PIN_ALIVE, 0));
    kvh("  gpa1 con ", rd32(PIN_ALIVE + 0x20), "  dat ", bank_dat(PIN_ALIVE, 1));
    kvh("  gpa2 con ", rd32(PIN_ALIVE + 0x40), "  dat ", bank_dat(PIN_ALIVE, 2));
    kvh("  gpp1 con ", rd32(PIN_PERIC0 + 0x20), "  dat ", bank_dat(PIN_PERIC0, 1));
    color(DIM);
    puts_("  alive dat 0x00..0xe0: ");
    color(FG);
    for (u32 b = 0; b < 8; b++) {
        hex(bank_dat(PIN_ALIVE, b), 2);
        putch(' ');
    }
    putch('\n');
    // (USI sw_conf at 0x10411004.. aborts on a read: secure-only, not read here.)
    // Buttons and the touch attention line, live for 20 s: press each button
    // and touch the screen; counters show every change seen.
    color(HEAD);
    puts_("Press vol+ vol- bixby power, touch the screen (5 s)\n");
    int row = cur_y;
    u32 cnt[5] = { 0 }, last[5], v[5];
    u32 prev[6], ev = 0;
    for (u32 b = 0; b < 6; b++)
        prev[b] = bank_dat(PIN_ALIVE, b);
    for (int i = 0; i < 5; i++)
        last[i] = 2;
    u64 end = now() + 5 * TB_HZ;     // buttons proven (probe #5): short now
    while (now() < end) {
        u32 a0 = bank_dat(PIN_ALIVE, GPA0), a1 = bank_dat(PIN_ALIVE, GPA1), a2 = bank_dat(PIN_ALIVE, GPA2);
        v[0] = (a0 >> 3) & 1; v[1] = (a0 >> 4) & 1; v[2] = (a0 >> 6) & 1;
        v[3] = (a2 >> 4) & 1; v[4] = a1 & 1;
        cur_y = row;
        cur_x = 0;
        for (int i = 0; i < 5; i++) {
            if (last[i] != 2 && v[i] != last[i])
                cnt[i]++;
            last[i] = v[i];
            color(v[i] ? DIM : OK);
            // no table of string pointers: the shim runs where S-Boot put it,
            // and initialised pointers hold link-time (0-based) addresses
            switch (i) {
            case 0: puts_("vol+ "); break;
            case 1: puts_("vol- "); break;
            case 2: puts_("bixby "); break;
            case 3: puts_("power "); break;
            default: puts_("t-irq "); break;
            }
            color(FG);
            dec(v[i]);
            putch('/');
            dec(cnt[i]);
            puts_(" ");
        }
        // every bit change in alive banks 0..5, in order (first 24): bank.bit=new
        for (u32 b = 0; b < 6; b++) {
            u32 d = bank_dat(PIN_ALIVE, b), x = d ^ prev[b];
            for (u32 bit = 0; bit < 8 && x; bit++)
                if (x & (1u << bit)) {
                    if (ev < 24) {
                        cur_y = row + 1 + ev / 6;
                        cur_x = (ev % 6) * 7;
                        color(FG);
                        dec(b);
                        putch('.');
                        dec(bit);
                        putch('=');
                        dec((d >> bit) & 1);
                    }
                    ev++;
                }
            prev[b] = d;
        }
        wait_ticks(TB_HZ / 50);
    }
    cur_y = row + 5;
    cur_x = 0;
    putch('\n');

    // Everything below may abort if the block is gated or secure: the
    // exception screen then shows where. Button results are already shown.
    // (pre-init HSI2C dump dropped in probe #9: it was all zeros and the
    // screen ran out of lines)
    touch_power();
    bb_test();
    wr32(BB_CON, (rd32(BB_CON) & ~(0xffu << 16)) | (0x22u << 16));   // back to I2C function

    // Touch chip: I2C pins to their I2C function (gpp1-4/5 = 2), then
    // write command 0x52 (device ID) and read 3 bytes.
    color(HEAD);
    puts_("Touch I2C (0x48, cmd 0x52)\n");
    u32 con = rd32(PIN_PERIC0 + 0x20);
    con = (con & ~(0xffu << 16)) | (0x22u << 16);
    wr32(PIN_PERIC0 + 0x20, con);
    kvh("  gpp1 con ", rd32(PIN_PERIC0 + 0x20), "  dat ", bank_dat(PIN_PERIC0, 1));
    // Probe #5: USI out of reset but HSI2C registers still read 0 and ignore
    // writes: its clocks are off. CMU_PERIC0 at 0x10400000 (as on Exynos
    // 8895, Linux clk-exynos8895.c): gate registers 0x2000.., bit 20 MANUAL,
    // bit 21 CG_VALUE (on), bit 28 ENABLE_HWACG. Like Linux
    // exynos_arm64_init_clocks + clk_enable: manual mode, clock on, for every
    // gate-shaped register (only bits 20/21/28/29 in use).
    color(HEAD);
    puts_("CMU_PERIC0 0x10400000 (safe reads)\n");
    {
        const u64 cmu = 0x10400000UL;
        // map: which 0x100-byte pages answer, and which word offsets in
        // 0x2000..0x21fc are gate-shaped (bits 20/21/28/29 only)
        color(DIM);
        puts_("  pages ");
        color(FG);
        for (u32 pg = 0; pg < 0x40; pg++) {
            safe_faults = 0;
            (void)safe_rd32(cmu + pg * 0x100);
            putch(safe_faults ? '.' : '#');
        }
        putch('\n');
        u32 n = 0, on = 0, miss = 0, col = 0;
        for (u32 o = 0x2000; o < 0x3000; o += 4) {
            safe_faults = 0;
            u32 v = safe_rd32(cmu + o);
            if (safe_faults) {
                miss++;
                continue;
            }
            if (v & ~0x30300000u)
                continue;
            n++;
            if (!(v & (1u << 21)) || (v & (1u << 28))) {
                if (col < 16) {
                    color(DIM);
                    hex(o & 0xfff, 3);
                    putch(':');
                    color(FG);
                    hex(v >> 20, 3);
                    putch(' ');
                    if (++col % 4 == 0)
                        putch('\n');
                }
                safe_wr32(cmu + o, (v & ~(1u << 28)) | 1u << 20 | 1u << 21);
                on++;
            }
        }
        putch('\n');
        kvh("  gates ", n, "  switched on ", on);
        kvh("  missing ", miss, "", 0);
    }

    // Probe #7: all PERIC0 gates found were already on, the USI wrapper
    // answers (+0xcc version 0x2070000) but the HSI2C registers read 0: the
    // USI is not in I2C mode. Its mode is SYSREG_PERIC0 USI SW_CONF, for this
    // bus usi10 = 0x1041102C (stock DT: usi_v2_mode "i2c"); Linux exynos-usi:
    // SW_CONF I2C = BIT(2). Earlier a plain read of 0x10411004 aborted.
    color(HEAD);
    puts_("SYSREG_PERIC0 usi10 sw_conf 0x1041102C\n");
    {
        safe_faults = 0;
        u32 a = safe_rd32(0x10410000UL);
        u32 fa = safe_faults;
        safe_faults = 0;
        u32 b = safe_rd32(0x1041102CUL);
        u32 fb = safe_faults;
        kvh("  +0000 ", a, fa ? "  FAULT  +102c " : "  +102c ", b);
        kvh("  faults ", fa << 4 | fb, "", 0);
        safe_faults = 0;
        safe_wr32(0x1041102CUL, 1u << 2);
        u32 fw = safe_faults;
        safe_faults = 0;
        u32 c = safe_rd32(0x1041102CUL);
        kvh("  after write ", c, "  faults w/r ", fw << 4 | safe_faults);
        // Probe #8: 0x1041102C took the value but hsi2c10 stayed dead. The
        // stock DT has two I2C-mode USIs (0x1041101C, 0x1041102C) and two
        // enabled HSI2Cs (0x104B0000, 0x104C0000); by position 0x1041101C
        // (index 6 from 0x10411004) is 0x104B0000. Both are I2C in Android.
        safe_faults = 0;
        u32 d0 = safe_rd32(0x1041101CUL);
        safe_wr32(0x1041101CUL, 1u << 2);
        u32 d1 = safe_rd32(0x1041101CUL);
        kvh("  101c before ", d0, "  after ", d1);
        kvh("  faults ", safe_faults, "", 0);
    }
    // USI v2 wrapper (+0xc4 USI_CON bit0 = reset, +0xc8 USI_OPTION):
    // release the reset, clock request on, clock stop off (Linux exynos-usi).
    wr32(HSI2C10 + 0xc4, rd32(HSI2C10 + 0xc4) | 1u);     // reset after the mode change
    wait_ticks(TB_HZ / 1000);
    wr32(HSI2C10 + 0xc8, (rd32(HSI2C10 + 0xc8) & ~(1u << 2)) | (1u << 1));
    wr32(HSI2C10 + 0xc4, rd32(HSI2C10 + 0xc4) & ~1u);
    wait_ticks(TB_HZ / 1000);
    kvh("  usi con ", rd32(HSI2C10 + 0xc4), "  option ", rd32(HSI2C10 + 0xc8));
    hs_init();
    kvh("  ctl ", rd32(HSI2C10 + HS_CTL), "  conf ", rd32(HSI2C10 + HS_CONF));   // full dump: probe #9
    // Probe #10: the touch supplies were off and are on now (L35 0x2c ->
    // 0xec, L43 0x30 -> 0xf0, SCL/SDA high), but 0x48 gave NO_DEV_ACK.
    // Give the chip time, watch its attn line, then scan the whole bus.
    for (int i = 0; i < 10; i++) {
        wait_ticks(TB_HZ / 10);
        putch((bank_dat(PIN_ALIVE, GPA1) & 1) ? 'H' : 'L');
    }
    putch('\n');
    ipclk_fix();
    // Probe #11: attn stays low (event pending: the chip is up), 0x48 read
    // aborted without NO_DEV_ACK -> bus timing. Try Samsung's init with four
    // timings; one ID read (write 0x52, read 3) each.
    u32 r1 = 1, r2 = 1, got = 0;
    u8 id[3] = { 0 };
    for (int mode = 0; mode < 4; mode++) {
        hs_samsung(mode);
        u8 cmd = 0x52;
        id[0] = id[1] = id[2] = 0;
        got = 0;
        r1 = hs_msg(0x48, 0, &cmd, 1, 0);
        r2 = hs_msg(0x48, 1, id, 3, &got);
        color(DIM);
        puts_("  m");
        dec(mode);
        puts_(" fs ");
        color(FG);
        hex(rd32(HSI2C10 + HS_FS1) >> 16 & 0xff, 2);
        putch('/');
        hex(rd32(HSI2C10 + HS_FS2) & 0xff, 2);
        putch('/');
        hex(rd32(HSI2C10 + HS_FS3) >> 16 & 0xff, 2);
        color(DIM);
        puts_(" w ");
        color(FG);
        hexv(r1);
        color(DIM);
        puts_(" r ");
        color(FG);
        hexv(r2);
        color(DIM);
        puts_(" id ");
        color(got == 3 ? OK : FG);
        hexv((u32)id[0] << 16 | id[1] << 8 | id[2]);
        putch('\n');
        if (!(r1 & 0x1e) && !(r2 & 0x1e) && got == 3)
            break;
    }
    r1 = (r1 & 0x1e) ? r1 : 0;
    r2 = (r2 & 0x1e) || got < 3 ? r2 | 0x100 : 0;
    color(!r1 && !r2 ? OK : BAD);
    puts_(!r1 && !r2 ? "  TOUCH CHIP ANSWERS\n" : "  no answer\n");
}
#endif


// ---------------------------------------------------------------- GPU probe (t7)
// Mali-G72 (Bifrost) at 0x17500000. Power-up as Samsung's 9810 PMUCAL
// (flexpmu_cal_local_exynos9810.h embedded_g3d_on): CMU_G3D 0x17400000 PLL /
// mux, PMU EMBEDDED_G3D_CONFIGURATION 0x14064040 = 0xF, wait STATUS 0x14064044,
// then the DTZPC restore SMC (0x82000410, WAKEUP_PD_DOWN, 0x17410204, group 2)
// that opens the block to non-secure software. All accesses fault-tolerant.
#ifdef GPUTEST
#define CMU_G3D 0x17400000UL
#define PMU_G3D 0x14064040UL
#define MALI    0x17500000UL

static u64 smc4(u64 a0, u64 a1, u64 a2, u64 a3)
{
    register u64 x0 __asm__("x0") = a0;
    register u64 x1 __asm__("x1") = a1;
    register u64 x2 __asm__("x2") = a2;
    register u64 x3 __asm__("x3") = a3;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
                     "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                     "x14", "x15", "x16", "x17", "memory");
    return x0;
}

static u32 srd(u64 a, u32 *fault)
{
    safe_faults = 0;
    u32 v = safe_rd32(a);
    if (fault)
        *fault = safe_faults;
    return v;
}

static void mali_regs(const char *when)
{
    u32 f;
    color(HEAD);
    puts_(when);
    putch('\n');
    u32 id = srd(MALI + 0x000, &f);
    kvh("  GPU_ID ", id, "  fault ", f);
    if (f)
        return;
    kvh("  L2 ", srd(MALI + 0x004, 0), "  TILER ", srd(MALI + 0x00c, 0));
    kvh("  MEM ", srd(MALI + 0x010, 0), "  MMU ", srd(MALI + 0x014, 0));
    kvh("  AS_PRESENT ", srd(MALI + 0x018, 0), "  JS_PRESENT ", srd(MALI + 0x01c, 0));
    kvh("  STATUS ", srd(MALI + 0x034, 0), "  IRQ_RAW ", srd(MALI + 0x020, 0));
    kvh("  SHADER_PRESENT ", srd(MALI + 0x100, 0), "  L2_PRESENT ", srd(MALI + 0x120, 0));
    kvh("  TILER_PRESENT ", srd(MALI + 0x110, 0), "  SHADER_READY ", srd(MALI + 0x140, 0));
    kvh("  L2_READY ", srd(MALI + 0x160, 0), "  TILER_READY ", srd(MALI + 0x150, 0));
}


// First GPU job: Mali MMU address space 0 in AArch64 4K mode (as Linux
// panfrost_mmu.c: TRANSCFG ADRMODE_AARCH64_4K | INA_BITS(55-48) |
// PTW_MEMATTR_WB | PTW_RA, ARM LPAE stage-1 tables), identity-mapping the
// 2 MiB block at GPU_MEM; then one WRITE_VALUE job (type 2, Immediate32) on
// job slot 1. GPU memory is non-cacheable on both sides (MMU off here,
// MEMATTR 0x4c), so no cache maintenance is needed.
#define GPU_MEM   0xef200000UL
#define GPU_PT0   (GPU_MEM + 0x0000)
#define GPU_PT1   (GPU_MEM + 0x1000)
#define GPU_PT2   (GPU_MEM + 0x2000)
#define GPU_JOB   (GPU_MEM + 0x10000)
#define GPU_DST   (GPU_MEM + 0x20000)
#define AS0       (MALI + 0x2400)
#define JOBCTRL   (MALI + 0x1000)
#define JS1       (MALI + 0x1800 + 0x80)

static void wr64(u64 a, u64 v) { wr32(a, (u32)v); wr32(a + 4, (u32)(v >> 32)); }

static int as0_wait(void)
{
    u64 t0 = now();
    while (srd(AS0 + 0x28, 0) & 1)                  // AS_STATUS ACTIVE
        if (now() - t0 > TB_HZ / 100)
            return -1;
    return 0;
}

static void gpu_first_job(void)
{
    color(HEAD);
    puts_("First GPU job (MMU AS0 + WRITE_VALUE)\n");
    for (u64 a = GPU_MEM; a < GPU_MEM + 0x21000; a += 4)
        wr32(a, 0);
    // identity map of the 2 MiB block holding GPU_MEM (L0 -> L1 -> L2 block)
    u64 va = GPU_MEM & ~0x1fffffUL;
    wr64(GPU_PT0 + 8 * ((va >> 39) & 511), GPU_PT1 | 3);
    wr64(GPU_PT1 + 8 * ((va >> 30) & 511), GPU_PT2 | 3);
    wr64(GPU_PT2 + 8 * ((va >> 21) & 511), va | 0x741);   // block: AF, SH inner, AP rw, attr 0
    __asm__ volatile("dsb sy" ::: "memory");

    // AS0: lock-free setup, then UPDATE
    wr64(AS0 + 0x00, GPU_PT0);                                  // TRANSTAB
    wr64(AS0 + 0x08, 0x4c4c4c4c4c4c4c4cULL);                    // MEMATTR: non-cacheable
    wr64(AS0 + 0x30, 6u | (55u - 48u) << 6 | 2u << 24 | 1u << 30);  // TRANSCFG
    wr32(AS0 + 0x18, 1);                                        // COMMAND UPDATE
    kvh("  AS0 update ", (u32)as0_wait(), "  status ", srd(AS0 + 0x28, 0));

    // job descriptor: 32-byte header + write-value payload
    wr32(GPU_JOB + 0x10, 1u | 2u << 1);        // 64-bit descriptor, job_type WRITE_VALUE
    wr32(GPU_JOB + 0x10, (1u | 2u << 1) | (1u << 16));   // job_index 1
    wr64(GPU_JOB + 0x18, 0);                   // next_job
    wr64(GPU_JOB + 0x20, GPU_DST);             // payload: address
    wr32(GPU_JOB + 0x28, 6);                   // Immediate 32
    wr64(GPU_JOB + 0x30, 0xc0ffee42);          // immediate value
    __asm__ volatile("dsb sy" ::: "memory");

    wr32(JOBCTRL + 0x004, 0xffffffff);                          // JOB_IRQ_CLEAR
    wr64(JS1 + 0x40, GPU_JOB);                                  // HEAD_NEXT
    wr64(JS1 + 0x50, srd(MALI + 0x100, 0));                     // AFFINITY_NEXT
    wr32(JS1 + 0x58, 0u | 3u << 8 | 3u << 12 | 8u << 16);       // CONFIG_NEXT: AS0, flushes, prio
    wr32(JS1 + 0x60, 1);                                        // COMMAND_NEXT START
    u64 t0 = now();
    u32 raw = 0;
    while (now() - t0 < TB_HZ / 10) {
        raw = srd(JOBCTRL + 0x000, 0);
        if (raw & (1u << 1 | 1u << 17))
            break;
    }
    kvh("  JOB_IRQ_RAW ", raw, "  us ", (now() - t0) * 1000000 / TB_HZ);
    kvh("  JS1 status ", srd(JS1 + 0x24, 0), "  hdr status ", rd32(GPU_JOB + 0x00));
    kvh("  AS0 fault ", srd(AS0 + 0x1c, 0), "  addr ", srd(AS0 + 0x20, 0));
    u32 got = rd32(GPU_DST);
    kvh("  memory ", got, "  want ", 0xc0ffee42);
    color(got == 0xc0ffee42 ? OK : BAD);
    puts_(got == 0xc0ffee42 ? "  GPU EXECUTED A JOB\n" : "  job did not write\n");
}

static void gpu_test(void)
{
    u32 f;
    color(HEAD);
    puts_("GPU probe (t7) Mali-G72\n");
    kvh("  PMU G3D conf ", srd(PMU_G3D, 0), "  status ", srd(PMU_G3D + 4, &f));
    kvh("  CMU_G3D mux ", srd(CMU_G3D + 0x100, &f), "  fault ", f);
    kvh("  PLL_CON0 ", srd(CMU_G3D + 0x140, 0), "  PLL_CON2 ", srd(CMU_G3D + 0x148, 0));
    // (pre-power-up register dump dropped: probe #1 showed G3D already on)

    if ((srd(PMU_G3D + 4, 0) & 0xf) != 0xf) {
        // Samsung's sequence. PLL output only if the PLL is enabled (bit 31)
        // and locked (bit 29); else the GPU stays on OSCCLK (26 MHz).
        u32 pll = srd(CMU_G3D + 0x140, &f);
        if (!f && (pll & (1u << 31)) && (pll & (1u << 29))) {
            safe_wr32(CMU_G3D + 0x148, 0x30000003);
            safe_wr32(CMU_G3D + 0x140, pll | 1u << 4);
            safe_wr32(CMU_G3D + 0x100, srd(CMU_G3D + 0x100, 0) | 1u << 4);
            puts_("  clock: PLL_G3D\n");
        } else {
            puts_("  clock: OSCCLK (PLL off)\n");
        }
        safe_wr32(PMU_G3D, (srd(PMU_G3D, 0) & ~0xfu) | 0xf);
        u64 t0 = now();
        while ((srd(PMU_G3D + 4, 0) & 0xf) != 0xf && now() - t0 < TB_HZ / 10)
            ;
        kvh("  power on: status ", srd(PMU_G3D + 4, 0), "  ms ", (now() - t0) * 1000 / TB_HZ);
    }
    u64 r = smc4(0x82000410UL, 1, 0x17410204UL, 2);
    kvh("  TZPC restore smc ", r, "", 0);

    // Power up L2, tiler and shader cores (Bifrost: *_PWRON_LO at 0x1a0/0x190/0x180)
    u32 sp = srd(MALI + 0x100, &f), tp = srd(MALI + 0x110, 0), lp = srd(MALI + 0x120, 0);
    if (!f && sp && sp != 0xdeadbeef) {
        safe_wr32(MALI + 0x1a0, lp);
        wait_ticks(TB_HZ / 100);
        safe_wr32(MALI + 0x190, tp);
        safe_wr32(MALI + 0x180, sp);
        wait_ticks(TB_HZ / 20);
        mali_regs("cores on:");
        color(srd(MALI + 0x140, 0) == sp ? OK : BAD);
        puts_(srd(MALI + 0x140, 0) == sp ? "  ALL SHADER CORES READY\n" : "  cores not ready\n");
    }
    if (srd(MALI + 0x140, 0) == sp)
        gpu_first_job();
}
#endif


// ---------------------------------------------------------------- SMP probe (t8)
// cpus=4 under XNU: PSCI CPU_ON returned 0 but cpu 1 never ran. Is it the
// entry address above 4 GiB (XNU runs at 0x8_8000_0000)? Start cpu 1 at the
// shim's own (low) smp_entry and cpu 2 at a copy in the high bank; each
// writes a marker to the mailbox at 0xef300000.
#ifdef SMPTEST
extern char smp_entry[], smp_entry_end[];
#define SMP_MBOX 0xef300000UL
#define SMP_HIGH 0x880100000UL

static u64 psci(u64 fn, u64 a1, u64 a2, u64 a3)
{
    register u64 x0 __asm__("x0") = fn;
    register u64 x1 __asm__("x1") = a1;
    register u64 x2 __asm__("x2") = a2;
    register u64 x3 __asm__("x3") = a3;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
                     "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                     "x14", "x15", "x16", "x17", "memory");
    return x0;
}

#define PMU 0x14060000UL
static u32 core_status_off(u64 mpidr)
{
    u32 c = (mpidr >> 8) & 0xff, n = mpidr & 0xff;
    return c == 0 ? 0x2024 + n * 8 : 0x2004 + n * 8;
}

static void smp_try(u64 mpidr, u64 entry, u32 ctx)
{
    u32 slot = (mpidr & 0xff) + ((mpidr >> 8) & 0xff) * 4;
    u64 mb = SMP_MBOX + slot * 16;
    for (int i = 0; i < 16; i += 4)
        wr32(mb + i, 0);
    __asm__ volatile("dsb sy" ::: "memory");
    u32 so = core_status_off(mpidr);
    u32 st0 = safe_rd32(PMU + so);
    u64 rc = psci(0xC4000003UL, mpidr, entry, ctx);
    u64 t0 = now();
    while (!rd32(mb) && now() - t0 < 2 * TB_HZ)
        ;
    u32 m = rd32(mb);
    kvh("  rc ", (u32)rc, "  mbox ", m);
    kvh("  x0 ", rd32(mb + 8), "  status ", st0);
    kvh("  -> ", safe_rd32(PMU + so), "", 0);
    color(m >> 16 == 0x5ec0 ? OK : BAD);
    puts_(m >> 16 == 0x5ec0 ? "  CORE RAN\n" : "  core did not run\n");
    color(FG);
}

static void smp_test(void)
{
    u64 el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    color(HEAD);
    puts_("SMP probe 2 (t8)\n");
    kvh("  boot cpu EL ", (u32)(el >> 2), "", 0);
    for (int i = 0; i < 4; i++)
        kvh("  little st ", safe_rd32(PMU + 0x2024 + i * 8), "  big st ", safe_rd32(PMU + 0x2004 + i * 8));
    kvh("  BID_EN ", safe_rd32(0x14070200UL), "  EVT_EN ", safe_rd32(PMU + 0x7f08));
    color(HEAD);
    puts_("cpu 1 low entry\n");
    smp_try(1, (u64)smp_entry, 0x11);
    color(HEAD);
    puts_("cpu 4 (big 0x100) low entry\n");
    smp_try(0x100, (u64)smp_entry, 0x44);
    u32 n = (u32)(smp_entry_end - smp_entry);
    for (u32 i = 0; i < n; i += 4)
        wr32(SMP_HIGH + i, rd32((u64)smp_entry + i));
    __asm__ volatile("dsb sy; isb" ::: "memory");
    color(HEAD);
    puts_("cpu 2 high entry\n");
    smp_try(2, SMP_HIGH, 0x22);
    color(HEAD);
    puts_("all mailbox slots:\n");
    for (int i = 0; i < 8; i += 2)
        kvh("  ", rd32(SMP_MBOX + i * 16), "  ", rd32(SMP_MBOX + (i + 1) * 16));
}
#endif

// ---------------------------------------------------------------- exceptions
void shim_exception(u64 vec, u64 esr, u64 elr, u64 far, u64 spsr)
{
    cur_fg = BAD;
    puts_("\n*** EXCEPTION vector ");
    dec(vec);
    puts_(" ***\n");
    kv("  ESR  ", esr);
    kv("  ELR  ", elr);
    kv("  FAR  ", far);
    kv("  SPSR ", spsr);
#ifndef HOST
    for (;;)
        __asm__ volatile("wfe");
#endif
}

// ---------------------------------------------------------------- main
void shim_main(u64 x0, u64 x1, u64 x2, u64 x3, u64 load)
{
    rect(0, 0, FB_W, FB_H, BG);
#ifdef SMPTEST
    smp_test();
    color(HEAD);
    puts_("\nDone. Seconds since start:\n");
    counter((cur_y + 1) * CH + 40);
    for (;;)
        __asm__ volatile("wfe");
#endif
#ifdef GPUTEST
    gpu_test();
    color(HEAD);
    puts_("\nDone. Seconds since start:\n");
    counter((cur_y + 1) * CH + 40);
    for (;;)
        __asm__ volatile("wfe");
#endif
#ifdef INPUTTEST
    input_test();
    color(HEAD);
    puts_("\nDone. Seconds since start:\n");
    counter((cur_y + 1) * CH + 40);
    for (;;)
        __asm__ volatile("wfe");
#endif

    color(HEAD);
    puts_("S9 shim  -  Exynos 9810\n");

    kv("  load addr   ", load);
    kv("  x0 (fdt)    ", x0);
    kv("  x1 x2 x3    ", x1 | x2 | x3);             // all three should be 0

    u64 el = (SYSREG(CurrentEL) >> 2) & 3;
    u64 sctlr = SYSREG(sctlr_el1);
    color(DIM);
    puts_("  EL ");
    color(el == 1 ? OK : WARN);
    dec(el);
    color(DIM);
    puts_("  MMU ");
    color(FG);
    puts_(sctlr & 1 ? "on" : "off");
    color(DIM);
    puts_("  D$ ");
    color(FG);
    puts_(sctlr & 4 ? "on" : "off");
    color(DIM);
    puts_("  I$ ");
    color(FG);
    puts_(sctlr & (1 << 12) ? "on\n" : "off\n");

    color(HEAD);
    puts_("CPU\n");
    u64 midr = SYSREG(midr_el1);
    kv("  MIDR        ", midr);
    color(DIM);
    puts_("  core        ");
    color(FG);
    puts_(((midr >> 4) & 0xfff) == 0xd05 ? "Cortex-A55\n" : "other (not A55)\n");

    u64 mmfr0 = SYSREG(id_aa64mmfr0_el1);
    kv("  MMFR0       ", mmfr0);
    u64 tg16 = (mmfr0 >> 20) & 0xf, tg4 = (mmfr0 >> 28) & 0xf, pa = mmfr0 & 0xf;
    color(DIM);
    puts_("  16K pages   ");
    color(tg16 ? OK : BAD);
    puts_(tg16 ? "YES" : "NO");
    color(DIM);
    puts_("   4K ");
    color(FG);
    puts_(tg4 == 0 ? "yes" : "no");
    color(DIM);
    puts_("   PA ");
    color(FG);
    dec(pa == 0 ? 32 : pa == 1 ? 36 : pa == 2 ? 40 : pa == 3 ? 42 : pa == 4 ? 44 : 48);
    puts_(" bit\n");

    kv("  PFR0        ", SYSREG(id_aa64pfr0_el1));
    kv("  ISAR0       ", SYSREG(id_aa64isar0_el1));
    kv("  ISAR1       ", SYSREG(id_aa64isar1_el1));
    kv("  CTR         ", SYSREG(ctr_el0));
    kv("  CNTFRQ      ", SYSREG(cntfrq_el0));

#ifdef UFSTEST
    ufs_probe();
    color(HEAD);
    puts_("\nDone. Seconds since start:\n");
    counter((cur_y + 1) * CH + 40);
#endif
#ifdef GICTEST
    gic_test();
    color(HEAD);
    puts_("\nDone. Seconds since start:\n");
    counter((cur_y + 1) * CH + 40);
#endif
    // A linked-in XNU payload means: boot it. Returns only if it can't.
    boot_xnu();
#ifdef HOST
    if (payload_ok((const struct payload *)payload_start))
        return;                         // host test of the loader: stop here
#endif

    if (x0 && !(x0 & 7))
        show_fdt((const u8 *)x0);
    else {
        color(BAD);
        puts_("no valid FDT pointer in x0\n");
    }

    probe_bank();

    color(HEAD);
    puts_("\nDone. Seconds since start:\n");
    counter((cur_y + 1) * CH + 40);
}
