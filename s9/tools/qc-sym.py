#!/usr/bin/env python3
"""CI helper (qc-probe.yml): symbolicate unslid shared-cache addresses.

  qc-sym.py CACHE 'addr addr ...'

For each address: the image containing it (cache header text info) and the
nearest symbol at or below it (ipsw dyld symaddr --image, per image).
Prints notice annotations only.
"""
import os
import re
import struct
import subprocess
import sys


def images(path):
    with open(path, 'rb') as f:
        hdr = f.read(0x200)
        off, cnt = struct.unpack_from('<QI', hdr, 0x88)
        f.seek(off)
        recs = [struct.unpack('<16sQII', f.read(32)) for _ in range(cnt)]
        out = []
        for _uuid, addr, size, poff in recs:
            f.seek(poff)
            name = f.read(512).split(b'\0', 1)[0].decode()
            out.append((addr, size, name))
    return out


def main():
    cache = sys.argv[1]
    addrs = [int(a, 16) for a in sys.argv[2].split()]
    imgs = images(cache)
    syms = {}
    lines = []
    for a in addrs:
        img = next((i for i in imgs if i[0] <= a < i[0] + i[1]), None)
        if img is None:
            lines.append(f'{a:#x}: not in any image text')
            continue
        name = os.path.basename(img[2])
        if name not in syms:
            r = subprocess.run(['ipsw', 'dyld', 'symaddr', cache, '--image', name],
                               capture_output=True, text=True)
            table = []
            for l in r.stdout.splitlines():
                m = re.match(r'\s*0x([0-9a-fA-F]+):\s+\(([^)]*)\)\s+(\S+)', l)
                if m and '__text' in m.group(2):
                    table.append((int(m.group(1), 16), m.group(3)))
            table.sort()
            syms[name] = table
        best = None
        for s_addr, s_name in syms[name]:
            if s_addr <= a:
                best = (s_addr, s_name)
            else:
                break
        sym = f'{best[1]}+{a - best[0]:#x}' if best else '?'
        lines.append(f'{a:#x}: {name} {sym}')
    print('::notice title=qc-sym::' + '%0A'.join(lines))
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
