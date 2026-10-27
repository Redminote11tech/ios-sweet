#!/bin/bash
# Counts ARMv8.1 LSE atomics (CAS/LDADD/SWP...) in iOS core libraries of the
# dyld shared cache inside the iOS root pulled by pull-crashlogs/root-update
# (/root/ios-root/phone-root.img). Read-only. The S9's Mongoose M3 cores have
# no LSE: code using it cannot run there. Run by scan-lse.bat in WSL.
set -uo pipefail
HERE=$PWD
IMG=/root/ios-root/phone-root.img
MNT=/mnt/iosroot-lse
[ -f "$IMG" ] || { echo "RESULT: NO_IMAGE ($IMG; run pull-crashlogs.bat first)"; exit 1; }
grep -qw hfsplus /proc/filesystems || modprobe hfsplus || { echo "RESULT: NO_HFSPLUS"; exit 1; }
mkdir -p "$MNT"
umount "$MNT" 2>/dev/null || true
mount -t hfsplus -o loop,ro,force "$IMG" "$MNT" || { echo "RESULT: MOUNT_FAILED"; exit 1; }
trap 'umount "$MNT" 2>/dev/null || true' EXIT
CACHE=$(find "$MNT/System/Library/Caches" "$MNT/private/preboot" "$MNT/System/Cryptexes" -name dyld_shared_cache_arm64 -type f 2>/dev/null | head -1)
[ -n "$CACHE" ] || { echo "RESULT: NO_CACHE"; find "$MNT" -maxdepth 6 -name 'dyld_shared_cache*' 2>/dev/null | head; exit 1; }
echo "cache: $CACHE"
ls -la "$(dirname "$CACHE")" | head -40
cp "$HERE/qc-verify.py" "$HERE/qc-sym.py" "$HERE/qc-lse.py" /tmp/ 2>/dev/null
python3 -I /tmp/qc-lse.py "$CACHE" | grep -v "^::"
echo "RESULT: SCANNED"
