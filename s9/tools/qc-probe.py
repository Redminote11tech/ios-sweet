#!/usr/bin/env python3
"""CI helper for qc-probe.yml: QuartzCore addresses in a dyld shared cache.

  qc-probe.py CACHE 'regex1,regex2'

Reports, as GitHub notice annotations (chunked):
  - QuartzCore symbols matching the sweetgl hook names / tex_fragment_shaders
  - the first 16 bytes of each of those functions (hook prologue check)
  - the cache address of the string colorMatrix4x4Bias(s0)
  - disassembly of the symbols matching the given regexes
"""
import re
import subprocess
import sys

HOOKS = ['emit_filter', 'render_vibrant_color_matrix', 'render_vibrant_filter', 'filter_node',
         'MaskNode5apply', 'prepare_filters', 'emit_filter_quads_color', 'render_background',
         'render_solid_background', 'fill_round_rect', 'fill_color_rect', 'create_shader',
         'tex_fragment_shaders', 'set_surface', 'variable_masked_blur']


def run(*args):
    r = subprocess.run(['ipsw', *args], capture_output=True, text=True)
    return r.stdout + r.stderr


def notice(title, text):
    text = text.replace('%', '%25').replace('\r', '').replace('\n', '%0A')
    step = 60000
    for i in range(0, max(len(text), 1), step):
        print(f'::notice title={title} {i // step}::{text[i:i + step]}')


def main():
    cache = sys.argv[1]
    regexes = [r for r in (sys.argv[2] if len(sys.argv) > 2 else '').split(',') if r]
    syms = run('dyld', 'symaddr', cache, '--image', 'QuartzCore')
    lines = [l for l in syms.splitlines() if any(h in l for h in HOOKS)]
    print(f'{len(syms.splitlines())} QuartzCore symbol lines, {len(lines)} matching')
    notice('symbols', '\n'.join(lines[:400]))
    addrs = []
    for l in lines:
        m = re.search(r'0x([0-9a-fA-F]{9,})', l)
        if m:
            addrs.append((int(m.group(1), 16), l))
    out = []
    for a, l in addrs:
        d = run('dyld', 'disass', cache, '--vaddr', hex(a), '--count', '4')
        out.append(f'{a:#x} {l.strip()[-90:]}\n' + '\n'.join(x for x in d.splitlines() if '0x' in x)[:600])
    notice('prologues', '\n'.join(out))
    notice('string', run('dyld', 'str', cache, 'colorMatrix4x4Bias(s0)', '--image', 'QuartzCore')[-3000:]
           + run('dyld', 'str', cache, 'colorMatrix4x4Bias(s0)')[-3000:])
    for rx in regexes:
        for a, l in addrs:
            if re.search(rx, l):
                d = run('dyld', 'disass', cache, '--vaddr', hex(a), '--count', '3000')
                body = []
                for x in d.splitlines():
                    body.append(x)
                    if re.search(r'\bret\b', x) and len(body) > 50 and 'b\t' not in x:
                        pass
                notice(f'disass {rx} {a:#x}', '\n'.join(body))


if __name__ == '__main__':
    main()
