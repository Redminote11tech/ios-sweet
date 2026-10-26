#!/bin/bash
# Copies iOS's own crash reports (.ips, with exception reasons and
# symbolicated-ready backtraces) out of the phone's iOS root. Read-only on
# the phone. Run by pull-crashlogs.bat in WSL, phone in TWRP.
# Result: crashlogs\ next to this script. Log: pull-crashlogs.log
set -euo pipefail
HERE=$PWD
ADB="$HERE/adb.exe"
DEV=/dev/block/by-name/USERDATA
SIZE=$((16 << 30))
WORK=/root/ios-root
IMG=$WORK/phone-root.img
MNT=/mnt/iosroot-ro
OUT=$HERE/crashlogs
mkdir -p "$WORK" "$MNT" "$OUT"
grep -qw hfsplus /proc/filesystems || modprobe hfsplus || { echo "RESULT: NO_HFSPLUS (run use-hfs-kernel.bat)"; exit 3; }
"$ADB" devices
"$ADB" shell 'umount /data 2>/dev/null; umount /sdcard 2>/dev/null; umount /data/media 2>/dev/null; true'
sig=$("$ADB" exec-out "dd if=$DEV bs=1024 skip=1 count=1 2>/dev/null" | head -c 2)
[ "$sig" = "HX" ] || [ "$sig" = "H+" ] || { echo "RESULT: NO_IOS_ROOT_ON_USERDATA (signature '$sig')"; exit 1; }
echo "=== $(date +%T) pulling the root from the phone (16 GiB, about 10-15 minutes)"
rm -f "$IMG"
"$ADB" exec-out "dd if=$DEV bs=4194304 count=$((SIZE / 4194304)) 2>/dev/null" | dd of="$IMG" bs=4M iflag=fullblock status=progress
[ "$(stat -c %s "$IMG")" = "$SIZE" ] || { echo "RESULT: PULL_SHORT"; exit 1; }
umount "$MNT" 2>/dev/null || true
mount -t hfsplus -o loop,ro,force "$IMG" "$MNT"
trap 'umount "$MNT" 2>/dev/null || true' EXIT
n=0
for d in private/var/mobile/Library/Logs/CrashReporter private/var/root/Library/Logs/CrashReporter \
	private/var/mobile/Library/Logs/CrashReporter/Retired var/mobile/Library/Logs/CrashReporter; do
	[ -d "$MNT/$d" ] || continue
	echo "--- $d"
	ls -la "$MNT/$d" | tail -40
	for f in "$MNT/$d"/*.ips "$MNT/$d"/*.ips.ca.synced "$MNT/$d"/*.crash; do
		[ -f "$f" ] || continue
		cp "$f" "$OUT/" && n=$((n + 1))
	done
done
echo "RESULT: COPIED $n crash reports to crashlogs"
