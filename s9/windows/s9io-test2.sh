#!/bin/bash
# Read-only on USERDATA. Narrows down the s9io write problem (writes go to
# RAM files in TWRP's /tmp only).
set -u
ADB="$PWD/adb.exe"
DEV=/dev/block/by-name/USERDATA
W=/root/ios-root
T=$(mktemp -d)
python3 -c "import sys; sys.stdout.buffer.write(bytes(i & 255 for i in range(1048576)))" > $T/pat.bin
sha1sum $T/pat.bin
"$ADB" push "$(wslpath -w "$PWD/s9io")" /tmp/s9io >/dev/null
"$ADB" push "$(wslpath -w "$T/pat.bin")" /tmp/pat.bin
"$ADB" shell 'chmod 755 /tmp/s9io; sha1sum /tmp/pat.bin 2>/dev/null || busybox sha1sum /tmp/pat.bin'
echo "== A: on the phone, s9io w from a local file"
"$ADB" shell 'dd if=/dev/zero of=/tmp/a.bin bs=1048576 count=3 2>/dev/null; /tmp/s9io w /tmp/a.bin 1048576 < /tmp/pat.bin; echo exit=$?; /tmp/s9io r /tmp/a.bin 1048576 1048576 | busybox sha1sum; ls -la /tmp/a.bin; busybox hexdump -C /tmp/a.bin | head -8'
echo "== B: exec-in into s9io"
"$ADB" shell 'dd if=/dev/zero of=/tmp/b.bin bs=1048576 count=3 2>/dev/null'
cat $T/pat.bin | "$ADB" exec-in "/tmp/s9io w /tmp/b.bin 1048576"
"$ADB" shell '/tmp/s9io r /tmp/b.bin 1048576 1048576 | busybox sha1sum; ls -la /tmp/b.bin; busybox hexdump -C /tmp/b.bin | head -8'
echo "== C: exec-in into cat"
cat $T/pat.bin | "$ADB" exec-in "cat > /tmp/c.bin"
"$ADB" shell 'ls -la /tmp/c.bin; busybox sha1sum /tmp/c.bin; busybox hexdump -C /tmp/c.bin | head -4'
echo "== D: phone MiB 0 vs the updated image's MiB 0"
"$ADB" exec-out "/tmp/s9io r $DEV 0 1048576" </dev/null > $T/phone0.bin
dd if=$W/phone-root.img bs=1048576 count=1 status=none > $T/img0.bin
cmp -l $T/phone0.bin $T/img0.bin | wc -l
cmp -l $T/phone0.bin $T/img0.bin | head -20
hexdump -C $T/phone0.bin | sed -n '64,72p'
hexdump -C $T/img0.bin | sed -n '64,72p'
