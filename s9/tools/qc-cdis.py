#!/usr/bin/env python3
"""CI helper (qc-probe.yml): disassemble shared-cache code at IMAGE+offset
(crash report imageOffset) with string/selector/CFString literals resolved.

  qc-cdis.py CACHE 'Image:off[,before,after] ...'
"""
import os
import struct
import sys

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import import_module  # noqa: E402

Cache = import_module('qc-verify').Cache
images = import_module('qc-sym').images
M = 0xFFFFFFFFF


def cstr(c, va):
    b = c.read(va, 160) or b''
    s = b.split(b'\0', 1)[0]
    if len(s) < 2 or any(ch < 0x20 or ch > 0x7e for ch in s):
        return None
    return s.decode()


def describe(c, va):
    s = cstr(c, va)
    if s:
        return repr(s)
    b = c.read(va, 32)
    if not b:
        return ''
    p0, _flags, p2, ln = struct.unpack('<QQQQ', b)
    s = cstr(c, p2 & M)
    if s and 0 < ln < 4096:
        return f'@"{s}"'
    s = cstr(c, p0 & M)
    if s:
        return f'*-> {s!r}'
    return ''


def stub_name(c, md, va, symcache, imgs):
    """objc_msgSend$sel stubs -> '[sel]'; GOT stubs -> image symbol."""
    code = c.read(va, 32)
    if not code:
        return ''
    ins = list(md.disasm(code, va))
    regs = {}
    sel = None
    for i in ins[:8]:
        ops = i.operands
        if i.mnemonic == 'adrp':
            regs[ops[0].reg] = ops[1].imm
        elif i.mnemonic.startswith('ldr') and len(ops) == 2 and ops[1].type == ARM64_OP_MEM and ops[1].mem.base in regs:
            slot = regs[ops[1].mem.base] + ops[1].mem.disp
            raw = c.read(slot, 8)
            ptr = struct.unpack('<Q', raw)[0] & M if raw else 0
            if i.op_str.startswith('x1,'):
                sel = cstr(c, ptr)
            elif i.op_str.startswith('x16,') or i.op_str.startswith('x17,'):
                return f'[{sel}]' if sel else symbolize(c, ptr, symcache, imgs)
        elif i.mnemonic in ('br', 'braa', 'brab', 'ret'):
            break
    return f'[{sel}]' if sel else ''


def symbolize(c, va, symcache, imgs):
    import re
    import subprocess
    img = next((i for i in imgs if i[0] <= va < i[0] + i[1]), None)
    if img is None:
        return f'{va:#x}'
    name = os.path.basename(img[2])
    if name not in symcache:
        r = subprocess.run(['ipsw', 'dyld', 'symaddr', sys.argv[1], '--image', name], capture_output=True, text=True)
        t = []
        for l in r.stdout.splitlines():
            m = re.match(r'\s*0x([0-9a-fA-F]+):\s+\(([^)]*)\)\s+(\S+)', l)
            if m:
                t.append((int(m.group(1), 16), m.group(3)))
        symcache[name] = sorted(t)
    best = None
    for a, n in symcache[name]:
        if a <= va:
            best = (a, n)
        else:
            break
    return f'{name}!{best[1]}+{va - best[0]:#x}' if best else name


def main():
    c = Cache(sys.argv[1])
    imgs = images(sys.argv[1])
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    out = []
    symcache = {}
    for spec in sys.argv[2].split():
        name, rest = spec.split(':')
        parts = [int(x, 0) for x in rest.split(',')]
        off = parts[0]
        before = parts[1] if len(parts) > 1 else 0x200
        after = parts[2] if len(parts) > 2 else 0x20
        img = next(i for i in imgs if os.path.basename(i[2]) == name)
        base = img[0]
        code = c.read(base + off - before, before + after)
        out.append(f'--- {name} {img[2]} +{off:#x} (va {base + off:#x})')
        regs = {}
        for ins in md.disasm(code, base + off - before):
            ops = ins.operands
            note = ''
            if ins.mnemonic == 'adrp':
                regs[ops[0].reg] = ops[1].imm
            elif ins.mnemonic == 'add' and len(ops) == 3 and ops[1].reg in regs and ops[2].type == ARM64_OP_IMM:
                regs[ops[0].reg] = regs[ops[1].reg] + ops[2].imm
                note = describe(c, regs[ops[0].reg])
            elif ins.mnemonic.startswith('ldr') and len(ops) == 2 and ops[1].type == ARM64_OP_MEM \
                    and ops[1].mem.base in regs:
                note = describe(c, regs[ops[1].mem.base] + ops[1].mem.disp)
            elif ins.mnemonic == 'bl':
                note = f'-> {stub_name(c, md, ops[0].imm, symcache, imgs)}'
            mark = '>>' if ins.address == base + off else '  '
            out.append(f'{mark}+{ins.address - base:#07x} {ins.mnemonic} {ins.op_str}' + (f'   ; {note}' if note else ''))
    keep = out if os.environ.get('QC_ALL') else [l for l in out if l.startswith('---') or l.startswith('>>') or ';' in l
            or any(k in l for k in (' cbz ', ' cbnz ', ' tbz ', ' tbnz ', ' b.', ' brk ', ' ret'))]
    text = '\n'.join(keep)
    for i in range(0, len(text), 3500):
        print(f'::notice title=qc-cdis {i // 3500}::' + text[i:i + 3500].replace('\n', '%0A'))
    print('\n'.join(out))


if __name__ == '__main__':
    main()
