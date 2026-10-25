# S9 (Exynos 9810) working notes

Handoff log for the Galaxy S9 port. Newest first. Observations are marked as such; guesses are marked as guesses.

## 2026-10-25

- **Shim T0 passed (observed):** S-Boot jumps to byte 0 of the boot image kernel at EL1. Watchdogs off with `WTCON=0` at `0x10050000`/`0x10060000` (no reset in 12+ min). Display updates only after `0x1281` -> `0x16030070` (DECON HW trigger). Framebuffer `0xCC000000` XRGB8888 stride 5760 confirmed.
- **Shim T2 passed (observed):** EL1, Cortex-A55 r0p1, TGran16 supported, PA 40 bit, ISAR1 no PAC, PFR0 `0x10112222` (GIC field 0: no ICC_* sysregs), CNTFRQ 0, FDT at `0x8a000000`, load address varies per boot, 4 memory banks (4015 MiB), 2 GiB bank at `0x880000000` read/write OK (sparse 2048/2048, dense 16 MiB).
- **Kernel patches 0082 (GICv2) and 0083 (boot_args framebuffer)** pushed; first CI build `40177226600` (workflow_dispatch: pushes from this session did not start a run).
- **Shim loader:** `s9/shim` links the mkpayload payload into the image and loads it into the 2 GiB bank (by-address cache clean before copying, MMU off, x0 = boot_args).
- Next: build `xnu-Image` from the CI kernel, flash, photograph the screen. Gate: XNU console text.
- Phone screen has an old water-damage defect (garbage at boot sometimes); test screens drew cleanly.
