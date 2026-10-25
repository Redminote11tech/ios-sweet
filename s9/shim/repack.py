#!/usr/bin/env python3
"""Replace the kernel inside a stock Android boot.img, keeping everything else.

    python3 repack.py BOOT.img t0-Image shim-boot.img
    python3 repack.py --info BOOT.img

Supports boot image header versions 0, 1 and 2 (what the Galaxy S9 uses).
The ramdisk, second stage, recovery DTBO and DTB are copied unchanged, so S-Boot
sees the same layout as the stock image. Recomputes the SHA-1 id like mkbootimg
and appends Samsung's SEANDROIDENFORCE marker.
"""
import hashlib
import struct
import sys

HDR = struct.Struct("<8s10I16s512s32s1024s")   # v0 header fields
V1 = struct.Struct("<IQI")                     # recovery_dtbo_size, recovery_dtbo_offset, header_size
V2 = struct.Struct("<IQ")                      # dtb_size, dtb_addr


def pad(n, page):
    return (page - n % page) % page


def parse(data):
    if data[:8] != b"ANDROID!":
        sys.exit("not an Android boot image (no ANDROID! magic)")
    f = list(HDR.unpack_from(data, 0))
    h = dict(zip(["magic", "kernel_size", "kernel_addr", "ramdisk_size", "ramdisk_addr",
                  "second_size", "second_addr", "tags_addr", "page_size", "header_version",
                  "os_version", "name", "cmdline", "id", "extra_cmdline"], f))
    ver = h["header_version"]
    if ver > 2:
        sys.exit(f"header version {ver} is not supported by this script")
    off = HDR.size
    if ver >= 1:
        h["recovery_dtbo_size"], h["recovery_dtbo_offset"], h["header_size"] = V1.unpack_from(data, off)
        off += V1.size
    if ver >= 2:
        h["dtb_size"], h["dtb_addr"] = V2.unpack_from(data, off)
    page = h["page_size"]

    pos = page                                  # header occupies the first page
    parts = {}
    for name in ["kernel", "ramdisk", "second"] + (["recovery_dtbo"] if ver >= 1 else []) + (["dtb"] if ver >= 2 else []):
        size = h[name + "_size"]
        parts[name] = data[pos:pos + size]
        pos += size + pad(size, page)
    h["_end"] = pos
    return h, parts


def info(path):
    h, parts = parse(open(path, "rb").read())
    for k in ["header_version", "page_size", "kernel_addr", "ramdisk_addr", "second_addr", "tags_addr", "os_version"]:
        print(f"{k:16} {h[k]:#x}")
    print(f"{'name':16} {h['name'].rstrip(bytes(1))!r}")
    print(f"{'cmdline':16} {h['cmdline'].rstrip(bytes(1)).decode(errors='replace')!r}")
    for k, v in parts.items():
        print(f"{k + ' size':16} {len(v)}")
    print(f"{'kernel magic':16} {parts['kernel'][0x38:0x3c]!r}  (b'ARMd' = raw arm64 Image)")


def build(stock, kernel, out):
    data = open(stock, "rb").read()
    h, parts = parse(data)
    parts["kernel"] = open(kernel, "rb").read()
    ver, page = h["header_version"], h["page_size"]

    h["kernel_size"] = len(parts["kernel"])
    sha = hashlib.sha1()
    order = ["kernel", "ramdisk", "second"] + (["recovery_dtbo"] if ver >= 1 else []) + (["dtb"] if ver >= 2 else [])
    for name in order:
        sha.update(parts[name])
        sha.update(struct.pack("<I", len(parts[name])))
    h["id"] = sha.digest().ljust(32, b"\0")

    if ver >= 1 and h["recovery_dtbo_size"]:
        pos = page
        for name in ["kernel", "ramdisk", "second"]:
            n = len(parts[name])
            pos += n + pad(n, page)
        h["recovery_dtbo_offset"] = pos

    hdr = HDR.pack(h["magic"], h["kernel_size"], h["kernel_addr"], h["ramdisk_size"], h["ramdisk_addr"],
                   h["second_size"], h["second_addr"], h["tags_addr"], page, ver, h["os_version"],
                   h["name"], h["cmdline"], h["id"], h["extra_cmdline"])
    if ver >= 1:
        hdr += V1.pack(h["recovery_dtbo_size"], h["recovery_dtbo_offset"], h["header_size"])
    if ver >= 2:
        hdr += V2.pack(h["dtb_size"], h["dtb_addr"])

    img = bytearray(hdr + bytes(pad(len(hdr), page)))
    for name in order:
        img += parts[name] + bytes(pad(len(parts[name]), page))
    img += b"SEANDROIDENFORCE"
    open(out, "wb").write(img)
    print(f"wrote {out}: {len(img)} bytes, kernel {h['kernel_size']} bytes, header v{ver}, page {page}")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--info":
        info(sys.argv[2])
    elif len(sys.argv) == 4:
        build(*sys.argv[1:])
    else:
        sys.exit(__doc__)
