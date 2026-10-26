#!/bin/bash
# Adds or replaces files in the iOS root on the phone's USERDATA without
# reflashing all 16 GiB. Run by root-update.bat in WSL, phone in TWRP.
#
#  1. pulls the current root from the phone (16 GiB, the phone's own copy:
#     iOS has written to it since it was flashed)
#  2. fsck (replays the HFS journal), mounts it, copies the files listed in
#     rootfiles/install.txt ("SOURCE DEST MODE" per line, SOURCE relative to
#     rootfiles/), unmounts, fsck again
#  3. writes back only the 1 MiB chunks that changed (s9io: 64-bit offsets,
#     TWRP's dd cannot seek past 4 GiB), reads them back to check
#  4. records the disk extents of each installed file in
#     rootfiles/extents-<name>.txt, so a same-size file can later be
#     replaced in place (root-push-file.sh) without any of this
#
# Log: root-update.log next to this script.
set -euo pipefail
HERE=$PWD
ADB="$HERE/adb.exe"
DEV=/dev/block/by-name/USERDATA
SIZE=$((16 << 30))
WORK=/root/ios-root
IMG=$WORK/phone-root.img
MNT=/mnt/iosroot
FILES=$HERE/rootfiles
CH=$((1 << 20))
mkdir -p "$WORK" "$MNT"
[ -s "$FILES/install.txt" ] || { echo "no rootfiles/install.txt"; exit 1; }
command -v fsck.hfsplus >/dev/null || apt-get install -y -q hfsprogs >/dev/null
grep -qw hfsplus /proc/filesystems || modprobe hfsplus || { echo "RESULT: NO_HFSPLUS (run use-hfs-kernel.bat)"; exit 3; }

free=$(df --output=avail -B1 "$WORK" | tail -1)
[ -f "$IMG" ] && free=$((free + $(stat -c %s "$IMG")))
if [ "$free" -lt $((SIZE + (1 << 30))) ]; then
	echo "RESULT: NO_SPACE (WSL has $((free >> 30)) GiB free, needs 17)"
	exit 1
fi
"$ADB" devices
"$ADB" push "$(wslpath -w "$HERE/s9io")" /tmp/s9io >/dev/null
"$ADB" shell chmod 755 /tmp/s9io
"$ADB" shell 'umount /data 2>/dev/null; umount /sdcard 2>/dev/null; umount /data/media 2>/dev/null; true'
sig=$("$ADB" exec-out "dd if=$DEV bs=1024 skip=1 count=1 2>/dev/null" | head -c 2)
[ "$sig" = "HX" ] || [ "$sig" = "H+" ] || { echo "RESULT: NO_IOS_ROOT_ON_USERDATA (signature '$sig')"; exit 1; }

echo "=== $(date +%T) pulling the root from the phone (16 GiB, about 10-15 minutes)"
rm -f "$IMG"
"$ADB" exec-out "dd if=$DEV bs=4194304 count=$((SIZE / 4194304)) 2>/dev/null" | dd of="$IMG" bs=4M iflag=fullblock status=progress
got=$(stat -c %s "$IMG")
[ "$got" = "$SIZE" ] || { echo "RESULT: PULL_SHORT ($got bytes)"; exit 1; }

hashes() {      # one sha1 per 1 MiB chunk
	python3 -I - "$1" <<'EOF'
import hashlib, sys
with open(sys.argv[1], 'rb') as f:
    while True:
        b = f.read(1 << 20)
        if not b:
            break
        print(hashlib.sha1(b).hexdigest())
EOF
}
echo "=== $(date +%T) hashing"
hashes "$IMG" > "$WORK/before.sha1"

echo "=== $(date +%T) fsck, install files"
fsck.hfsplus -fy "$IMG" || true
mount -t hfsplus -o loop,force "$IMG" "$MNT"
trap 'umount "$MNT" 2>/dev/null || true' EXIT
sed 's/\r$//' "$FILES/install.txt" | while read -r src dest mode; do
	[ -n "$src" ] && [ "${src#\#}" = "$src" ] || continue
	echo "install $src -> $dest ($mode)"
	mkdir -p "$MNT$(dirname "$dest")"
	cp "$FILES/$src" "$MNT$dest"
	chmod "$mode" "$MNT$dest"
	chown 0:0 "$MNT$dest"
	sync
	# extents "file_block disk_start disk_end", 4 KiB blocks from the volume start (= USERDATA offset 0)
	filefrag -v -b4096 "$MNT$dest" | awk '/^ *[0-9]+:/ { gsub(/\.\./," "); gsub(/:/," "); print $2, $4, $5 }' \
		> "$FILES/extents-$(basename "$dest").txt"
	echo "  extents: $(wc -l < "$FILES/extents-$(basename "$dest").txt"), size $(stat -c %s "$MNT$dest")"
	stat -c %s "$MNT$dest" > "$FILES/size-$(basename "$dest").txt"
done
umount "$MNT"
trap - EXIT
fsck.hfsplus -fy "$IMG" || true
fsck.hfsplus -fn "$IMG" || echo "warning: fsck still reports issues"

echo "=== $(date +%T) writing back the changed chunks"
hashes "$IMG" > "$WORK/after.sha1"
paste -d' ' "$WORK/before.sha1" "$WORK/after.sha1" | awk '$1 != $2 { print NR - 1 }' > "$WORK/changed.txt"
echo "changed chunks: $(wc -l < "$WORK/changed.txt") MiB"
# runs of consecutive chunks, at most 64 per write
awk 'NR == 1 { s = $1; p = $1; next }
     $1 == p + 1 && $1 - s < 64 { p = $1; next }
     { print s, p - s + 1; s = $1; p = $1 }
     END { if (NR) print s, p - s + 1 }' "$WORK/changed.txt" > "$WORK/runs.txt"
while read -r start count; do
	dd if="$IMG" bs=$CH skip="$start" count="$count" status=none |
		"$ADB" exec-in "/tmp/s9io w $DEV $((start * CH))"
	want=$(dd if="$IMG" bs=$CH skip="$start" count="$count" status=none | sha1sum | cut -d' ' -f1)
	got=$("$ADB" exec-out "/tmp/s9io r $DEV $((start * CH)) $((count * CH))" </dev/null | sha1sum | cut -d' ' -f1)
	if [ "$want" != "$got" ]; then
		echo "RESULT: VERIFY_FAILED at MiB $start (+$count)"
		exit 1
	fi
	echo "  MiB $start +$count ok"
done < "$WORK/runs.txt"
"$ADB" shell sync
echo "RESULT: ROOT_UPDATED"
