# S9 (Exynos 9810) working notes

Handoff log for the Galaxy S9 port. Newest first. Observations are marked as such; guesses are marked as guesses.

## 2026-10-25

- **PHASE 3 COMPLETE (observed, 10:41 local):** with `qcom,kryo470` in the CPU compatibles, `SweetCPU: 1/1 CPUs initialized, SGI1 verified`, IORTC 30 s wait completes, calendar initialized, all of bsd_init runs (vfs, AMFI + Sandbox policies, HFS, mbuf, skywalk self-tests PASSED, loopback), then `Waiting on <dict ... IOMedia ... Apple_HFS>`. `Failed to write boot-picker-bringup-reason to NVRAM` is harmless.
- Next image: RAM-disk root (`rd=md0`, mockfs serves tools/sweet-init as /sbin/launchd), s9-image default from now on. xnu-Image 16736256 bytes.

- **Boot hang found (observed with `io=0xc startup_debug=1`, xnu run `40180209939`):** last bootstrap stage `PE_lockdown_iokit`; bsd_init never starts. Timer IRQs arrive (`SWEET IRQ: timer #1`..`#4096`). The CPU nubs `cpu0@0`..`cpu7@103` register but no driver starts on them: SweetCPU matches `IONameMatch 'qcom,kryo470'` only. Fix: the S9 ADT lists `qcom,kryo470` in each CPU's compatible.
- **t4 shim test (observed):** GIC-400 delivers virtual timer (30/30), physical timer (30/30) and SGI 1 (10/10) to EL1 with the kernel's GICv2 setup. GICD typer `0xfcef`, iidr `0x200143b`, GICC iidr `0x202143b`, itargets0 `0x01010101`. Stopwatch: shim counter 60 s = 60.46 s real, so 26 MHz is right.

- **PHASE 3 GATE PASSED (observed, photo 09:51 local): XNU runs on the S9 and prints to the screen.** First boot of `xnu-Image` (xnu run `40177226600`): `SWEET GIC: GICv2 (memory-mapped CPU interface)`, `cpu 0 GICv2 mask 0x1 timer INTID 27`, `Darwin Kernel Version 22.6.0 ... xnu-8796.141.3`, pmap/VM/zones, scheduler, IOKit, `Boot version: s9-shim`, AMFI, platform expert, AppleKeyStore, IOWatchdog, IOCoreSurfaceRoot, HID buttons + touch registered (stubs only, Qualcomm drivers off), `SWEET BATTERY static fallback`, `SWEET FB: AppleCLCD 1440x2960 stride 5760 at 0xcc000000`. Console wrapped to the top; `pe_arm_init_debug: failed to find cpu-debug-interface` (harmless). Timer interrupts evidently work (scheduler + IOKit threads progress). No root device given, so the expected end is "waiting for root device"; not yet confirmed.

- **Shim T0 passed (observed):** S-Boot jumps to byte 0 of the boot image kernel at EL1. Watchdogs off with `WTCON=0` at `0x10050000`/`0x10060000` (no reset in 12+ min). Display updates only after `0x1281` -> `0x16030070` (DECON HW trigger). Framebuffer `0xCC000000` XRGB8888 stride 5760 confirmed.
- **Shim T2 passed (observed):** EL1, Cortex-A55 r0p1, TGran16 supported, PA 40 bit, ISAR1 no PAC, PFR0 `0x10112222` (GIC field 0: no ICC_* sysregs), CNTFRQ 0, FDT at `0x8a000000`, load address varies per boot, 4 memory banks (4015 MiB), 2 GiB bank at `0x880000000` read/write OK (sparse 2048/2048, dense 16 MiB).
- **Kernel patches 0082 (GICv2) and 0083 (boot_args framebuffer)** pushed; first CI build `40177226600` (workflow_dispatch: pushes from this session did not start a run).
- **Shim loader:** `s9/shim` links the mkpayload payload into the image and loads it into the 2 GiB bank (by-address cache clean before copying, MMU off, x0 = boot_args).
- **First kernel built:** xnu run `40177226600` (0082+0083 compile). Kernel 17.2 MB, entry pa `0x88148c488`, kernel `0x881004000`-`0x8824c8000`, ADT `0x8824c8000`, boot_args `0x8824cc000`.
- **s9-image workflow** packages the image (artifact `s9-xnu-image` + branch `s9-builds`, which this session can fetch; artifact/log downloads are blocked here). Loader host test passes: all 10 segments, ADT and boot_args land where expected.
- `xnu-Image` 16.7 MB sent to the user for the first XNU boot. Gate: XNU console text. Expect the white band bottom-left (start.s stage 0) and "SWEET EARLY n" lines if the kernel starts.
- Phone screen has an old water-damage defect (garbage at boot sometimes); test screens drew cleanly.
