#!/bin/bash
# Writes /root/ios-root/ios16-root.img to the S9's USERDATA partition through
# TWRP (adb exec-in), then checks the first 4 MiB and the HFS+ signature.
# ERASES USERDATA. Run by flash-ios-root.bat.
set -euo pipefail
IMG=/root/ios-root/ios16-root.img
ADB="$PWD/adb.exe"
DEV=/dev/block/by-name/USERDATA
[ -s "$IMG" ] || { echo "no $IMG: run build-ios-root.bat first"; exit 1; }
"$ADB" devices
"$ADB" shell 'umount /data 2>/dev/null; umount /sdcard 2>/dev/null; umount /data/media 2>/dev/null; true'
"$ADB" shell "grep -q ' /data ' /proc/mounts && echo STILL_MOUNTED || echo data unmounted"
size=$(stat -c %s "$IMG")
echo "writing $((size >> 20)) MiB to USERDATA (about 10 minutes, do not unplug)"
dd if="$IMG" bs=4M status=progress | "$ADB" exec-in "dd of=$DEV bs=4194304"
"$ADB" shell sync
want=$(head -c 4194304 "$IMG" | sha256sum | cut -d' ' -f1)
got=$("$ADB" exec-out "dd if=$DEV bs=4194304 count=1 2>/dev/null" | sha256sum | cut -d' ' -f1)
sig=$("$ADB" exec-out "dd if=$DEV bs=1024 skip=1 count=1 2>/dev/null" | head -c 2)
echo "first 4 MiB: image $want"
echo "              phone $got"
echo "volume signature: $sig"
if [ "$want" = "$got" ] && [ "$sig" = "H+" ]; then
	echo "RESULT: USERDATA_WRITTEN_OK"
else
	echo "RESULT: VERIFY_FAILED"
	exit 1
fi
