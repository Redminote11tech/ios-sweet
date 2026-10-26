#!/usr/bin/env python3
"""CI helper (qc-probe.yml): disassemble an arm64 Mach-O around file-relative
text offsets (crash report imageOffset) and resolve adrp/add/ldr literals
to strings (C strings, selectors, CFStrings). Notice annotations only.

  qc-file.py BINARY 'off[,before,after] ...'
"""
import struct
import sys

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM

LC_SEGMENT_64 = 0x19


def load(path):
    data = open(path, 'rb').read()
    if struct.unpack_from('<I', data, 0)[0] == 0xcafebabe:  # fat: pick arm64
        n = struct.unpack_from('>I', data, 4)[0]
        for i in range(n):
            cpu, _sub, off, size, _al = struct.unpack_from('>5I', data, 8 + 20 * i)
            if cpu == 0x0100000c:
                data = data[off:off + size]
                break
    ncmds = struct.unpack_from('<I', data, 16)[0]
    p = 32
    segs = []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, p)
        if cmd == LC_SEGMENT_64:
            name = data[p + 8:p + 24].split(b'\0')[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, p + 24)
            nsects = struct.unpack_from('<I', data, p + 64)[0]
            sects = []
            for s in range(nsects):
                q = p + 72 + 80 * s
                sname = data[q:q + 16].split(b'\0')[0].decode()
                addr, ssize, off = struct.unpack_from('<QQI', data, q + 32)
                sects.append((sname, addr, ssize, off))
            segs.append((name, vmaddr, vmsize, fileoff, filesize, sects))
        p += size
    return data, segs


def va2off(segs, va):
    for _n, vm, vms, fo, fs, _s in segs:
        if vm <= va < vm + fs:
            return fo + va - vm
    return None


def sect_of(segs, va):
    for _n, _vm, _vms, _fo, _fs, sects in segs:
        for sname, addr, size, _off in sects:
            if addr <= va < addr + size:
                return sname
    return ''


def cstr(data, segs, va):
    o = va2off(segs, va)
    if o is None:
        return None
    s = data[o:o + 200].split(b'\0', 1)[0]
    try:
        return s.decode()
    except UnicodeDecodeError:
        return None


def describe(data, segs, va):
    sec = sect_of(segs, va)
    if sec in ('__cfstring',):
        o = va2off(segs, va)
        ptr = struct.unpack_from('<Q', data, o + 16)[0] & 0xffffffffff
        return f'@"{cstr(data, segs, ptr)}"'
    if sec in ('__objc_selrefs', '__objc_classrefs', '__got', '__objc_superrefs'):
        o = va2off(segs, va)
        ptr = struct.unpack_from('<Q', data, o)[0] & 0xffffffffff
        s = cstr(data, segs, ptr)
        return f'{sec} -> {s!r}' if s else f'{sec} -> {ptr:#x}'
    s = cstr(data, segs, va)
    return f'{sec} {s!r}' if s else sec


def main():
    data, segs = load(sys.argv[1])
    text = next(s for s in segs if s[0] == '__TEXT')
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    out = []
    for spec in sys.argv[2].split():
        parts = [int(x, 0) for x in spec.split(',')]
        off, before, after = (parts + [0x300, 0x40])[:3] if len(parts) == 1 else (parts + [0x40])[:3]
        va0 = text[1] + off - before
        code = data[off - before:off + after]
        out.append(f'--- offset {off:#x} (va {text[1] + off:#x})')
        regs = {}
        for ins in md.disasm(code, va0):
            note = ''
            ops = ins.operands
            if ins.mnemonic == 'adrp':
                regs[ops[0].reg] = ops[1].imm
            elif ins.mnemonic == 'add' and len(ops) == 3 and ops[1].reg in regs and ops[2].type == ARM64_OP_IMM:
                va = regs[ops[1].reg] + ops[2].imm
                regs[ops[0].reg] = va
                note = describe(data, segs, va)
            elif ins.mnemonic.startswith('ldr') and len(ops) == 2 and ops[1].type == ARM64_OP_MEM \
                    and ops[1].mem.base in regs:
                note = describe(data, segs, regs[ops[1].mem.base] + ops[1].mem.disp)
            mark = '>>' if ins.address == text[1] + off else '  '
            out.append(f'{mark}{ins.address - text[1]:#07x} {ins.mnemonic} {ins.op_str}' + (f'   ; {note}' if note else ''))
    print('::notice title=qc-file::' + '%0A'.join(out)[:60000])
    print('\n'.join(out))


if __name__ == '__main__':
    main()
