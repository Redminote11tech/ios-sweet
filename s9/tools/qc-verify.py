#!/usr/bin/env python3
"""CI helper for qc-probe.yml: checks sweetgl's QuartzCore addresses in a
dyld shared cache (main file + subcaches) by reading the bytes directly.

  qc-verify.py CACHE DELTA

DELTA is added to the ios-sailfish (iPhone 8) addresses in eagl.c. Prints
a short report (notice annotations), nothing else from the cache.
"""
import glob
import struct
import sys

SAILFISH_HOOKS = [
    (0x188424e54, (0xd103c3ff, 0x6d072beb, 0x6d0823e9, 0xa9096ffc), 'emit_filter'),
    (0x188427248, (0x6db923e9, 0xa9016ffc, 0xa90267fa, 0xa9035ff8), 'render_vibrant_color_matrix'),
    (0x188427018, (0xd102c3ff, 0x6d0533ed, 0x6d062beb, 0x6d0723e9), 'render_vibrant_filter'),
    (0x1883900d0, (0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd), 'filter_node'),
    (0x188372f74, (0xd10703ff, 0x6d1523e9, 0xa9166ffc, 0xa91767fa), 'MaskNode::apply'),
    (0x18834d888, (0x6db63bef, 0x6d0133ed, 0x6d022beb, 0x6d0323e9), 'prepare_filters'),
    (0x18834f480, (0xd10303ff, 0x6d0623e9, 0xa90767fa, 0xa9085ff8), 'emit_filter_quads_color'),
    (0x1882d30b8, (0xd10243ff, 0x6d0323e9, 0xa90467fa, 0xa9055ff8), 'render_background'),
    (0x188311534, (0x6db923e9, 0xa9016ffc, 0xa90267fa, 0xa9035ff8), 'render_solid_background'),
    (0x1884d504c, (0xd10483ff, 0x6d0d23e9, 0xa90e6ffc, 0xa90f57f6), 'fill_round_rect'),
    (0x1882f1298, (0xd10203ff, 0xa9045ff8, 0xa90557f6, 0xa9064ff4), 'fill_color_rect'),
    (0x1884af0d8, (0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6), 'create_shader'),
    (0x18840433c, (0xb90000bf, 0xb900009f, 0xb900007f, 0x390000df), 'format hook'),
]
SAILFISH_CASES = 0x18855a1e4
SAILFISH_TABLE = 0x1d5b44a10


class Cache:
    def __init__(self, path):
        self.maps = []
        for f in sorted(set([path] + glob.glob(path + '.*'))):
            if f.endswith('.map') or f.endswith('.symbols'):
                continue
            with open(f, 'rb') as fh:
                hdr = fh.read(0x200)
                if not hdr.startswith(b'dyld_v1'):
                    continue
                off, cnt = struct.unpack_from('<II', hdr, 0x10)
                fh.seek(off)
                for i in range(cnt):
                    addr, size, foff, _maxp, _initp = struct.unpack('<QQQII', fh.read(32))
                    self.maps.append((addr, size, foff, f))

    def read(self, va, n):
        for addr, size, foff, f in self.maps:
            if addr <= va and va + n <= addr + size:
                with open(f, 'rb') as fh:
                    fh.seek(foff + va - addr)
                    return fh.read(n)
        return None


def main():
    c = Cache(sys.argv[1])
    delta = int(sys.argv[2], 0)
    out = [f'{len(c.maps)} mappings, delta {delta:#x}']
    for a, want, name in SAILFISH_HOOKS:
        b = c.read(a + delta, 16)
        got = struct.unpack('<4I', b) if b else None
        out.append(f'{name}: {a + delta:#x} {"OK" if got == want else "MISMATCH " + str([hex(x) for x in got or []])}')
    cases = c.read(SAILFISH_CASES + delta - 0x11, 0x60)
    if cases:
        out.append('cases[0x11..]: ' + cases[0x11:0x40].hex())
        out.append(f'cases[0x13]={cases[0x13]:#x} [0x14]={cases[0x14]:#x} [0x26]={cases[0x26]:#x} '
                   f'[0x27]={cases[0x27]:#x} [0x2d]={cases[0x2d]:#x}')
    t = c.read(SAILFISH_TABLE + delta, 0x60 * 8)
    if t:
        ptrs = struct.unpack('<%dQ' % 0x60, t)
        out.append('table[0x13] raw %#x, null entries: %s' % (
            ptrs[0x13], ' '.join(hex(i) for i, p in enumerate(ptrs) if p == 0)))
    print('::notice title=qc-verify::' + '%0A'.join(out))
    print('\n'.join(out))


if __name__ == '__main__':
    main()
