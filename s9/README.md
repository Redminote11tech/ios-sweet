# ios-sweet on the Galaxy S9 (SM-G960F, Exynos 9810)

This folder ports [ios-sweet](https://github.com/dipa-studio/ios-sweet) (iOS 16 userland + Apple's open-source XNU on a Redmi Note 10 Pro) to the **Exynos** Galaxy S9 (`starlte`). Everything above the hardware is reused from ios-sweet; this folder holds what differs.

For the Snapdragon S9 (SM-G9600), see the separate [ios-s9](https://github.com/syncmodz/ios-s9) project instead.

## Status

| Phase | Gate | State |
| --- | --- | --- |
| 1. Recon | hardware facts, EFS backup | done |
| 2. Boot shim | shim draws on the S9 screen | done (T0, T2) |
| 3. XNU core | XNU console text on the S9 screen | done (PID 1 from a RAM disk) |
| 4. UFS + iOS root | Apple launchd from internal flash | in progress: XNU reads USERDATA (0086); iOS root image next |
| 5. iOS services | SpringBoard lock screen | not started |
| 6. Touch + buttons | unlock by swiping | not started |

## Hardware facts (measured on the phone)

| Item | Value |
| --- | --- |
| Entry from S-Boot | EL1, MMU off, D-cache bit on, x0 = FDT (`0x8a000000`); load address varies per boot (`0x80110000`, `0x80150000`) |
| CPU | cpu0-3 Cortex-A55 (MIDR `0x410fd051`), cpu4-7 Mongoose; 16K pages yes, PA 40 bit, no PAC, **no GIC system registers** (PFR0 `0x10112222`) |
| Timer | **CNTFRQ_EL0 = 0** (S-Boot never sets it): 26 MHz from the FDT; virtual timer PPI 11 = INTID 27 |
| GIC | GIC-400 (GICv2), GICD `0x10101000`, GICC `0x10102000` |
| RAM | `0x80000000` 968 MiB, `0xC0000000` 512 MiB, `0xE1900000` 487 MiB, **`0x880000000` 2 GiB (clean, used for XNU)**; probe OK |
| Display | S-Boot framebuffer `0xCC000000`, 1440x2960 XRGB8888, stride 5760; **write `0x1281` to DECON `0x16030070`** (HW trigger) or nothing reaches the panel |
| Watchdogs | `0x10050000` and `0x10060000`: `WTCON = 0` disables both |

## Kernel patches added for the S9

- `xnu/patches/0082-s9-gicv2.patch`: GICv2 selected at boot from `/arm-io/gic` compatible `arm,gic-400`; ack/EOI/SGI through the memory-mapped GICD/GICC.
- `xnu/patches/0083-s9-early-fb-from-boot-args.patch`: the early console and framebuffer mapping use `boot_args->Video` instead of the Redmi's fixed panel.
- `xnu/patches/0086-s9-exynos-ufs.patch`: Exynos UFS host quirks in `bsd/dev/sweet_ufs.c` (byte offsets, 4 KiB PRDT entries, nexus register, OCS 7, no HCE, START STOP UNIT to W-LUN 0xd0), enabled by `exynos-vs` in `/sweet-ufs`. Default partition `USERDATA`, read-only unless `ufsrw=1`.

## Building a boot image

1. **Kernel:** run the `xnu` workflow (Actions tab, or it runs on pushes to `xnu/patches/`), download the `xnu-kernel` artifact.
2. **Device tree:** `python3 s9/tools/mkadt_s9.py s9.adt`
3. **Payload:**
   ```
   python3 tools/mkpayload.py kernel.development.sweet s9.adt payload.bin \
     --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress sweetkeys=0 sweettouch=0 sweetusb=0 wdt=-1 wdt_test=no_user_checkin"
   ```
   `sweetkeys=0 sweettouch=0 sweetusb=0` matter: those drivers poke Qualcomm addresses that don't exist on Exynos.
4. **Shim with the payload linked in:** `sh s9/shim/build.sh payload.bin` produces `s9/shim/xnu-Image`. S-Boot loads the whole kernel image, so the payload rides inside it (no ramdisk).
5. **Boot image:** `python3 s9/shim/repack.py BOOT.img s9/shim/xnu-Image shim-boot.img`, where `BOOT.img` is your own backup of the BOOT partition.
6. **Flash from TWRP:** `adb push shim-boot.img /tmp/`, `adb shell dd if=/tmp/shim-boot.img of=/dev/block/by-name/BOOT`, `adb reboot`. Restore your `BOOT.img` the same way.

Without a payload, `sh s9/shim/build.sh` builds `t0-Image` (red screen) and `t2-Image` (diagnostics console).
