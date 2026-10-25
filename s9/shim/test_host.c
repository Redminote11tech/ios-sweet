// Host-side preview/test of the S9 shim (not used on the phone).
//   python3 mkdtb.py && cc -DHOST -O1 -o test_host test_host.c main.c
//   ./test_host                  diagnostics screen -> t2-preview.ppm
//   ./test_host payload.bin      runs the XNU loader against a 2 GiB host
//                                mapping at 0x880000000 (everything but the jump)
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

uint32_t host_fb[1440 * 2960];
const uint8_t *host_payload;
void shim_main(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

uint64_t host_sysreg(const char *n)
{
    static uint64_t t;
    if (!strcmp(n, "cntpct_el0")) return t += 13000000;
    if (!strcmp(n, "cntfrq_el0")) return 0;                 // as on the phone
    if (!strcmp(n, "CurrentEL")) return 1 << 2;
    if (!strcmp(n, "sctlr_el1")) return 0x30d0180c;
    if (!strcmp(n, "midr_el1")) return 0x410fd051;
    if (!strcmp(n, "id_aa64mmfr0_el1")) return 0x101122;
    if (!strcmp(n, "id_aa64pfr0_el1")) return 0x10112222;
    if (!strcmp(n, "ctr_el0")) return 0x84448004;
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t empty[64] __attribute__((aligned(16)));
    host_payload = empty;
    if (argc > 1) {
        FILE *p = fopen(argv[1], "rb");
        if (!p) { perror(argv[1]); return 1; }
        fseek(p, 0, SEEK_END);
        long n = ftell(p);
        fseek(p, 0, SEEK_SET);
        uint8_t *buf = aligned_alloc(16, (n + 15) & ~15L);
        if (fread(buf, 1, n, p) != (size_t)n) return 1;
        fclose(p);
        host_payload = buf;
        void *m = mmap((void *)0x880000000UL, 0x80000000UL, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
        if (m != (void *)0x880000000UL) { perror("mmap 0x880000000"); return 1; }
    }
    FILE *f = fopen("test.dtb", "rb");
    if (!f) { puts("run mkdtb.py first"); return 1; }
    static uint8_t dtb[65536] __attribute__((aligned(8)));
    if (!fread(dtb, 1, sizeof dtb, f)) return 1;
    fclose(f);

    shim_main((uint64_t)dtb, 0x80150000, 0, 0, 0x80150000);

    if (argc > 1) {
        // Check what the loader left behind: boot_args right after the ADT.
        const uint8_t *pl = host_payload;
        uint64_t virt_base, entry, adt_off, adt_size, kend, phys;
        memcpy(&virt_base, pl + 0x10, 8); memcpy(&entry, pl + 0x18, 8);
        memcpy(&adt_off, pl + 0x20, 8); memcpy(&adt_size, pl + 0x28, 8);
        memcpy(&kend, pl + 0x30, 8); memcpy(&phys, pl + 0x38, 8);
        uint64_t adt_pa = kend - virt_base + phys;
        uint64_t ba = (adt_pa + adt_size + 0x3fff) & ~0x3fffUL;
        printf("entry pa %#lx, ADT at %#lx (%s), boot_args at %#lx\n", entry - virt_base + phys, adt_pa,
               memcmp((void *)adt_pa, pl + adt_off, adt_size) ? "MISMATCH" : "matches", ba);
        uint64_t v[6];
        memcpy(v, (void *)(ba + 40), sizeof v);
        printf("Video base %#lx rowBytes %lu %lux%lu depth %lu; cmdline '%s'\n", v[0], v[2], v[3], v[4], v[5],
               (char *)(ba + 108));
        uint32_t nsegs; memcpy(&nsegs, pl + 12, 4);
        for (uint32_t i = 0; i < nsegs; i++) {
            uint64_t sg[4]; memcpy(sg, pl + 0x2a8 + 32 * i, 32);
            int ok = !memcmp((void *)(sg[0] - virt_base + phys), pl + sg[2], sg[3]);
            printf("  seg %u va %#lx size %#lx copied %s\n", i, sg[0], sg[1], ok ? "OK" : "MISMATCH");
        }
    }

    f = fopen("t2-preview.ppm", "wb");
    fprintf(f, "P6 1440 2960 255\n");
    for (int i = 0; i < 1440 * 2960; i++) {
        uint32_t p = host_fb[i];
        fputc(p >> 16, f); fputc(p >> 8, f); fputc(p, f);
    }
    fclose(f);
    puts("wrote t2-preview.ppm");
    return 0;
}
