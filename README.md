# iOS 16 on Galaxy S9

Oct 22, 2026 · @alphanCZE

## Summary

A Galaxy S9 port of [ios-sweet](https://github.com/dipa-studio/ios-sweet) is realistic, but it is a port of the method, not the code: roughly the upper half (iOS userland, kernel services, root image) carries over, and the lower half (boot shim, interrupts, storage, input, power) must be rewritten for the Exynos 9810.

ios-sweet compiles Apple's open-source XNU (xnu-8796) for a Snapdragon 732G phone and runs the unmodified iPhone X iOS 16.7.16 userland on it. Apple's signed boot chain is never involved: a custom shim, booted by the unlocked bootloader, loads XNU directly. It reached the SpringBoard home screen on 2026-10-18, with CPU-only rendering, no Wi-Fi/audio/GPU, and security (AMFI, sandbox, keystore) stubbed.

The S9 has two things going for it: its little cores are the same Cortex-A55 that ios-sweet runs on, and your mainline Linux work produces exactly the hardware facts (memory map, framebuffer, UFS, touch, keys) that the port needs.

Realistic scope: weeks of evenings to reach a kernel console, more to reach SpringBoard. The original's engineering log shows the UFS-to-home-screen stretch alone (15 to 18 October) as four long days, with AI agents doing much of the reverse engineering.

## Hardware: sweet vs S9

The CPU core and the iOS-facing layer match; nearly everything the kernel touches directly differs. S9 values marked *check* need confirming from your mainline DTS or the live device tree.

| Area | Redmi Note 10 Pro (sweet) | Galaxy S9 (starlte, Exynos 9810) | Port impact |
| --- | --- | --- | --- |
| Bootloader | Qualcomm ABL, `fastboot boot` (nothing flashed) | Samsung S-Boot, Download mode; boot partition must be flashed (Heimdall/Odin) | New boot.img packaging and flow |
| CPU used | 6x Cortex-A55 via PSCI, 16K pages, no PAC | 4x Cortex-A55 + 4x Mongoose M3 | Same A55 core; start on A55 only. SMP bring-up method *check* (PSCI or Samsung-specific) |
| Interrupts | GICv3, timer as Group 1 IRQ | GIC-400 (GICv2) *check* | Rewrite the interrupt-controller patch |
| RAM | \~3.95 GiB upper bank above Qualcomm carve-outs | 4 GB total, Samsung carve-outs *check* | New memory window in mkadt |
| Display | 1080x2400 at 0x9c000000, left by ABL | 1440x2960 framebuffer left by S-Boot, address *check* | Framebuffer address + centering 1125x2436 |
| Storage | UFS, Qualcomm host, ABL-initialized | UFS 2.1, Exynos host, S-Boot-initialized *check* | Same reuse-the-bootloader trick; Exynos register quirks |
| Touch | Goodix GT9896 over GENI I2C, power via RPMh | Samsung S6SY761 *check* over Exynos HSI2C | New I2C + touch driver; mainline has a reference driver |
| Buttons | PMIC over SPMI | GPIO keys on Exynos pinctrl *check* | Likely simpler |
| USB log | DWC3 + Qualcomm wrapper | DWC3 + Exynos wrapper *check* | DWC3 core reusable, wrapper init rewritten |
| Debug UART | None (wired as SPI) | Samsung UART over USB with a 619k JIG cable *check* | Possible bonus: earlier, easier logs |

## What carries over

About two thirds of the 77 kernel patches and all of the root-image work are SoC-independent and should apply unchanged. Grouping below follows the repo's patch table and notes.

| Patches / tools | What they do | S9 |
| --- | --- | --- |
| 0001-0009 | Toolchain, machine config, non-PAC arm64, Apple-internal stubs | Reuse; rename the machine config |
| 0010-0018 | Framebuffer console, physmap, PRNG, device-tree platform expert, timer PPI, boot CPU | Mostly rewrite: GICv2, timer, framebuffer address |
| 0019-0027 | libpthread, BearSSL AES, RAM-disk root, SHA, minimal AMFI, HFS+, MD5 | Reuse |
| 0028-0034 | UFS host driver, root device, read/write, DMA above 4 GiB | Rewrite host part for Exynos UFS; keep SCSI/GPT/root logic |
| 0035-0059 | Entitlements, bindfs, keystore stub, watchdog, IOSurface, IOMobileFramebuffer, sandbox policy, HID services, iPhone X identity | Reuse; this is the hard reverse-engineering, already done |
| 0060, 0062, 0063 | SPMI keys, RPMh touch power, GENI I2C + Goodix touch | Rewrite for Exynos GPIO keys, HSI2C, S9 touch IC |
| 0061 | Present display as 1125x2436 | Reuse; adjust centering for 1440x2960 |
| 0064, 0066-0071 | Work-interval grant, crash info, console mute, IOSurface lookup, log batching, pid\_hibernate, edge swipes | Reuse |
| 0065 | SMP via PSCI CPU\_ON | Check S9 CPU-on method first |
| 0072-0077 | DWC3 USB device mode, CDC-ECM live log | Reuse DWC3 core; rewrite Qualcomm VBUS override |
| shim/ | Bare-metal loader packed as Android boot.img v2 | Rewrite entry, DTB handoff and boot.img header for S-Boot |
| tools/mkadt.py | Apple Device Tree from MMIO layout + iPhone X identity | Reuse structure; new MMIO map, memory, framebuffer |
| tools/mkiosroot.sh, sweet-root-tweaks.py | iOS root image + bring-up tweaks | Reuse as-is |
| tools/qemu-sweet.py | QEMU test boots | Reuse for kernel-only changes |

## Port plan

Work top to bottom and don't start a phase until the previous gate passes; a failure is cheapest to debug at the lowest layer.

&#91;embedded content: S9 port plan · 7 phases, each with a gate\]

Phases 2, 3, 4 and 6 are new code; phase 5 is mostly applying ios-sweet's patches and root tweaks, which is where it spent most of its effort.

## Recon checklist (do first, nothing destructive)

Every item here comes from your mainline work or from TWRP; none of it touches the phone's data.

- [ ] Confirm OEM unlock is on and TWRP boots; note the exact model (SM-G960F/FD, Exynos)
- [ ] Back up EFS and modem partitions (`efs`, `sec_efs`, `cpefs` or your variant's names) plus `boot`, `recovery`, `dtbo` with TWRP or `dd`; keep them off any repo
- [ ] Dump the live device tree from Android or TWRP (`/sys/firmware/fdt`) and the stock boot.img
- [ ] Memory: `memory` nodes and every `reserved-memory` carve-out; pick the largest clean window for XNU
- [ ] Framebuffer: address, size, stride and format S-Boot leaves (the value your mainline `simple-framebuffer` uses)
- [ ] Interrupt controller: compatible string and register bases (expected `arm,gic-400`)
- [ ] Timer: arch timer frequency and PPI numbers
- [ ] CPU bring-up: `enable-method` on each CPU node (PSCI or vendor) and which core boots first
- [ ] Confirm the A55 cores support 16K pages (ID\_AA64MMFR0\_EL1.TGran16 from an early Linux boot)
- [ ] UFS: host base address, LUN layout, and the GPT position of `userdata`; check S-Boot leaves the host running
- [ ] Touch: controller model, I2C bus, address, interrupt GPIO, and which regulator powers it
- [ ] Buttons: power, volume and Bixby keys (GPIO or PMIC)
- [ ] USB: DWC3 base and the Exynos wrapper/PHY registers your mainline DTS uses
- [ ] Test whether a 619k JIG cable gives a usable UART console

## Risks and tooling

The biggest unknowns are low in the stack; the iOS side is the most de-risked part because ios-sweet already solved it.

**Risks**

- **Interrupts:** ios-sweet only taught XNU GICv3. A GICv2 path is new work and the first thing that can stall the port.
- **Memory:** 4 GB with Samsung carve-outs may leave a fragmented or small window; iOS needed about 4 GiB on sweet, and early sweet boots ran on 1.2 GiB.
- **Bootloader handoff:** S-Boot's boot.img format and DTB passing differ from ABL; no `fastboot boot` means every test is a flash.
- **Firmware protections:** an unknown secure-world (EL3) protection on some memory or registers shows up as an instant reset, as it might have on sweet.
- **EFS loss:** a bad partition write can destroy IMEI/calibration data. Back it up before anything else.
- **Data loss:** the iOS root replaces `userdata`, erasing Android's user data.
- **Legal:** Apple's license only permits iOS on Apple hardware. Download the IPSW from Apple for personal experimenting and never redistribute images.

**Tooling on your WSL setup**

- Kernel builds run on GitHub's macOS runners (free on a public fork), so no Mac needed.
- Root image: `apfs-fuse` and `hfsprogs` work in WSL; loop mounts need a WSL2 distro with root.
- Flashing: Heimdall in WSL needs USB passthrough (`usbipd-win`); Odin on Windows is the fallback. Writing the iOS root to `userdata` with `dd` from TWRP avoids both.
- Logs: the persistent log ring reads back from TWRP with `adb`, exactly as ios-sweet does from OrangeFox.

## Sources

- [ios-sweet repository and README](https://github.com/dipa-studio/ios-sweet)
- [ios-sweet NOTES.md engineering log](https://github.com/dipa-studio/ios-sweet/blob/main/NOTES.md)

S9 hardware values are from general knowledge, not sources; confirm each one in the recon checklist.
