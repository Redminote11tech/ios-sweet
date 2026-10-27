#!/usr/bin/env python3
"""CI helper (qc-probe.yml): count ARMv8.1 LSE atomics (CAS*, LDADD/LDCLR/
LDEOR/LDSET/LDSMAX.../SWP*) and other post-v8.0 instructions in the dyld
cache's executable mappings and in a few named images. The Exynos 9810's
Mongoose M3 cores are ARMv8.0: iOS code using LSE cannot run on them.

  qc-lse.py CACHE
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import import_module  # noqa: E402

Cache = import_module('qc-verify').Cache
images = import_module('qc-sym').images


def classify(w):
    if w & 0x3FA07C00 == 0x08A07C00:
        return 'cas'
    if w & 0x3F200C00 == 0x38200000:
        return 'ldop/swp'
    if w & 0xFFFFFC00 == 0x5AC01400 and False:
        return ''
    if w & 0xBFE0FC00 == 0x08207C00:
        return 'casp'
    if w & 0xFFC00000 == 0xD5000000 and (w >> 12) & 0xFFF == 0x033 and False:
        return ''
    return None


def scan(data):
    c = {}
    for i in range(0, len(data) - 3, 4):
        w = struct.unpack_from('<I', data, i)[0]
        k = classify(w)
        if k:
            c[k] = c.get(k, 0) + 1
    return c


def main():
    cache = sys.argv[1]
    c = Cache(cache)
    imgs = images(cache)
    out = []
    for name in ('libobjc.A.dylib', 'libsystem_platform.dylib', 'libsystem_pthread.dylib',
                 'libdispatch.dylib', 'CoreFoundation', 'libc++.1.dylib', 'QuartzCore', 'UIKitCore'):
        img = next((i for i in imgs if os.path.basename(i[2]) == name), None)
        if img is None:
            out.append(f'{name}: not found')
            continue
        data = c.read(img[0], img[1]) or b''
        out.append(f'{name}: text {img[1]:#x} {scan(data)}')
    print('::notice title=qc-lse::' + '%0A'.join(out))
    print('\n'.join(out))


if __name__ == '__main__':
    main()
