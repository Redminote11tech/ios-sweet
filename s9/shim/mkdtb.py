#!/usr/bin/env python3
"""Build a small test DTB with the S9's real memory banks, /chosen initrd and bootargs
(values copied from starlte-live.dts) for the host-side preview of the T2 shim."""
import struct

strings = bytearray()
def stroff(name):
    i = strings.find(name.encode() + b"\0")
    if i < 0:
        i = len(strings)
        strings.extend(name.encode() + b"\0")
    return i

body = bytearray()
def tok(t): body.extend(struct.pack(">I", t))
def begin(name):
    tok(1); body.extend(name.encode() + b"\0")
    while len(body) % 4: body.append(0)
def end(): tok(2)
def prop(name, val):
    tok(3); body.extend(struct.pack(">II", len(val), stroff(name))); body.extend(val)
    while len(body) % 4: body.append(0)
u32 = lambda *v: b"".join(struct.pack(">I", x) for x in v)

begin("")
prop("#address-cells", u32(2))
prop("#size-cells", u32(1))
prop("model", b"Samsung Star rev26 board based on EXYNOS9810\0")
begin("chosen")
prop("bootargs", b"console=ram loglevel=4 bootmode=2 s3cfb.bootloaderfb=0xcc000000 lcdtype=9503811 sales_code=BOG\0")
prop("linux,initrd-start", u32(0x89000000))
prop("linux,initrd-end", u32(0x89a66b75))
end()
for base, size in [(0x80000000, 0x3c800000), (0xC0000000, 0x20000000), (0xE1900000, 0x1E700000), (0x880000000, 0x80000000)]:
    begin(f"memory@{base:X}")
    prop("device_type", b"memory\0")
    prop("reg", u32(base >> 32, base & 0xffffffff, size))
    end()
end()
tok(9)

hdr_size, rsv = 40, 16
off_rsv = hdr_size
off_struct = off_rsv + rsv
off_str = off_struct + len(body)
total = off_str + len(strings)
hdr = struct.pack(">10I", 0xd00dfeed, total, off_struct, off_str, off_rsv, 17, 16, 0, len(strings), len(body))
open("test.dtb", "wb").write(hdr + bytes(rsv) + body + strings)
print("wrote test.dtb", total, "bytes")
