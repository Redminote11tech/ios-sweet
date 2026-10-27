#!/usr/bin/env python3
"""Apple Device Tree for the Galaxy S9 (SM-G960F, Exynos 9810, "starlte").

Same structure as tools/mkadt.py (the Redmi "sweet" tree) with the S9's
hardware, all taken from the live FDT (/sys/firmware/fdt) and the shim's
T0/T2 probes:

  RAM       4 banks; XNU gets the clean 2 GiB bank 0x8_8000_0000 (no carve-outs)
  GIC       GIC-400 (GICv2): GICD 0x10101000, GICC 0x10102000; the A55 has no
            GIC system registers, so the kernel uses its GICv2 path (patch 0082)
  Timer     26 MHz (CNTFRQ_EL0 reads 0: S-Boot never sets it), virtual timer
            PPI 11 = INTID 27
  CPUs      cpu0-3 Cortex-A55 MPIDR 0x000-0x003, cpu4-7 Mongoose 0x100-0x103
  Display   S-Boot framebuffer 0xCC000000, 1440x2960 XRGB8888, stride 5760

Usage:  mkadt_s9.py OUT.adt [--dump]
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '../../tools'))
import adt  # noqa: E402
from mkadt import (node, u32, u64, cstr, d22ap_product, BOOT_MANIFEST_HASH,  # noqa: E402
                   PANIC_LOG_SIZE, SOC_BASE)

PHYS_BASE = 0x880000000
MEM_SIZE = 0x80000000

GICD, GICD_SIZE = 0x10101000, 0x1000
GICC, GICC_SIZE = 0x10102000, 0x2000
TIMEBASE = 26000000
VTIMER_INTID = 27

FB, FB_SIZE = 0xCC000000, 0x1200000

# UFS host (s9/shim UFS probe #7 reads the GPT through it): standard UFSHCI
# registers plus the Exynos vendor HCI block (patch 0086).
UFS_HCI, UFS_HCI_SIZE = 0x11120000, 0x100
UFS_VS, UFS_VS_SIZE = 0x11121100, 0x100
FB_W, FB_H, FB_STRIDE = 1440, 2960, 1440 * 4

S9_ECID = 0x5339000000000001  # 'S9' + 1, synthetic (not the phone's)


def build(seed=None):
    phys_base, mem_size = PHYS_BASE, MEM_SIZE
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('s9-shim')),
        ('system-firmware-version', cstr('s9-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        ('display-scale', u32(3)),
        ('unique-chip-id', struct.pack('<Q', S9_ECID)),
        ('chip-id', u32(0x8015)),
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # All eight cores; the kernel starts cpus=N of them. 'reg' is what the core
    # reads in MPIDR_EL1 (Aff1:Aff0), matched by the kernel's reset vector: the
    # A55 cluster is DynamIQ (MT=1, Aff1 = core -> 0x000..0x300, measured with
    # the shim's SMP probe 6); the Mongoose cores are Aff1 = cluster, Aff0 = core.
    # 'psci-id' is the index the firmware's PSCI CPU_ON wants (1..3 start the
    # A55s; it accepts 0x100.. for the Mongooses but those still hang, see NOTES).
    MT_IDS = os.environ.get('S9_MT_IDS', '') == '1'
    cpu_nodes = []
    for core in range(8):
        big = core >= 4
        # Mongoose MPIDR Aff1:Aff0 = 0x10n collides with A55 core 1 (0x100):
        # XNU maps nodes to CPUs by 'reg' & 0xffff, so the big cores get an
        # unused placeholder (0xf0n) until they can actually be started.
        mpidr = (0xf00 | (core - 4)) if big else (core << 8)
        if MT_IDS:
            # SWEET_ARMV80 kernels (xnu patch 0106) match MPIDR & 0x1ffffff,
            # keeping the A55s' MT bit: A55 0x1000n00, M3 0x10n, no collision.
            mpidr = (0x100 | (core - 4)) if big else (0x1000000 | core << 8)
        psci_id = (0x100 | (core - 4)) if big else core
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            # 'qcom,kryo470' is what the SweetCPU driver matches (IONameMatch in
            # KernelConfigTables.cpp); without it no driver attaches to the CPU
            # nubs and the boot thread waits forever in PE_lockdown_iokit.
            ('compatible', cstr('samsung,mongoose-m3' if big else 'arm,cortex-a55') +
             cstr('qcom,kryo470') + cstr('ARM,v8')),
            ('reg', u32(mpidr)),
            ('psci-id', u32(psci_id)),
            # free low RAM for the CPU_ON trampoline: the firmware only starts
            # cores below 4 GiB, the kernel lives at 0x8_8000_0000 (NOTES).
            ('psci-trampoline', u64(0xef3f0000)),
            ('cpu-id', u32(core)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(1 if big else 0)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(TIMEBASE)),
            ('clock-frequency', u32(2704000000 if big else 1794000000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1794000000)),
            ('peripheral-frequency', u32(TIMEBASE)),
            ('fixed-frequency', u32(TIMEBASE)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-400') + cstr('arm,cortex-a15-gic')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(VTIMER_INTID)),
        # GICv2: <GICD base, size, GICC base, size>, relative to SOC_BASE
        ('reg', u64(GICD - SOC_BASE, GICD_SIZE, GICC - SOC_BASE, GICC_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])
    product_props = d22ap_product()
    if product_props:
        # iPhone X says P3: with the GLES renderer (sweetgl) CoreAnimation then
        # draws the display in 10-bit 'b3a8' surfaces, which neither Mesa's
        # import (eagl.c) nor the CPU present path handle (ios-ca1: black
        # screen, "attachImage: surface 2 b3a8 ... format failed"). sRGB keeps
        # the display surfaces 'BGRA' as with the software renderer.
        product_props = [(k, cstr('sRGB') if k == 'artwork-display-gamut' else v) for k, v in product_props]
    product = [node('product', [ph()] + product_props)] if product_props else []
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('samsung,exynos9810-ufs')),
        ('reg', u64(UFS_HCI, UFS_HCI_SIZE)),
        ('exynos-vs', u64(UFS_VS, UFS_VS_SIZE)),
    ])

    # Galaxy S9 buttons + S6SY761 touch (patch 0089, bsd/dev/s9_input.c):
    # the kernel driver runs only when this node exists.
    s9_input = node('s9-input', [
        ph(),
        ('compatible', cstr('samsung,starlte-input')),
    ])

    # Mali-G72 MP18 (patch 0094, bsd/dev/s9_mali.c): GPU registers. G3D power
    # domain, clocks and TZPC are handled in the driver (S-Boot leaves G3D on).
    s9_gpu = node('s9-gpu', [
        ph(),
        ('compatible', cstr('arm,mali-bifrost')),
        ('reg', u64(0x17500000, 0x5000)),
    ])

    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet') +
         cstr('samsung,starlte') + cstr('samsung,exynos9810') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, ufs, s9_input, s9_gpu, buttons, pram] + product)


if __name__ == '__main__':
    blob = adt.serialize(build())
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
