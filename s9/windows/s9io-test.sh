#!/bin/bash
# Read-only on USERDATA: checks s9io on the phone (TWRP) and which state
# the first MiB of the iOS root is in after the failed root-update write.
set -u
ADB="$PWD/adb.exe"
DEV=/dev/block/by-name/USERDATA
W=/root/ios-root
"$ADB" push "$(wslpath -w "$PWD/s9io")" /tmp/s9io >/dev/null
"$ADB" shell chmod 755 /tmp/s9io
echo "== s9io exists/runs:"
"$ADB" shell 'ls -la /tmp/s9io; /tmp/s9io; echo "exit=$?"' 2>&1
echo "== MiB 0 via dd vs s9io r:"
"$ADB" exec-out "dd if=$DEV bs=1048576 count=1 2>/dev/null" </dev/null | sha1sum
"$ADB" exec-out "/tmp/s9io r $DEV 0 1048576" </dev/null | sha1sum
echo "== expected: before / after"
sed -n 1p $W/before.sha1
sed -n 1p $W/after.sha1
echo "== write test on a RAM file:"
"$ADB" shell 'dd if=/dev/zero of=/tmp/t.bin bs=1048576 count=4 2>/dev/null; ls -la /tmp/t.bin'
head -c 1048576 /dev/urandom > /tmp/p.bin
sha1sum /tmp/p.bin
dd if=/tmp/p.bin bs=1048576 status=none | "$ADB" exec-in "/tmp/s9io w /tmp/t.bin 1048576"
echo "exec-in exit=$?"
"$ADB" exec-out "/tmp/s9io r /tmp/t.bin 1048576 1048576" </dev/null | sha1sum
"$ADB" exec-out "dd if=/tmp/t.bin bs=1048576 skip=1 count=1 2>/dev/null" </dev/null | sha1sum
echo "== uname / dd:"
"$ADB" shell 'uname -a; ls -la /system/bin/dd /sbin/dd 2>/dev/null; which toybox busybox' 2>&1
