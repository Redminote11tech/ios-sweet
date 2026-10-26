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
RESUME=0
[ "${1:-}" = "--writeback" ] && RESUME=1    # only redo step 3 from the last run's image
[ "${1:-}" = "--extents" ] && RESUME=2      # only step 4, from the last run's image
mkdir -p "$WORK" "$MNT"
[ -s "$FILES/install.txt" ] || { echo "no rootfiles/install.txt"; exit 1; }
command -v fsck.hfsplus >/dev/null || apt-get install -y -q hfsprogs >/dev/null
grep -qw hfsplus /proc/filesystems || modprobe hfsplus || { echo "RESULT: NO_HFSPLUS (run use-hfs-kernel.bat)"; exit 3; }

if [ $RESUME = 2 ]; then
	:
elif [ $RESUME = 0 ]; then
free=$(df --output=avail -B1 "$WORK" | tail -1)
[ -f "$IMG" ] && free=$((free + $(stat -c %s "$IMG")))
if [ "$free" -lt $((SIZE + (1 << 30))) ]; then
	echo "RESULT: NO_SPACE (WSL has $((free >> 30)) GiB free, needs 17)"
	exit 1
fi
fi
if [ $RESUME != 2 ]; then
"$ADB" devices
"$ADB" push "$(wslpath -w "$HERE/s9io")" /tmp/s9io >/dev/null
"$ADB" shell chmod 755 /tmp/s9io
"$ADB" shell 'umount /data 2>/dev/null; umount /sdcard 2>/dev/null; umount /data/media 2>/dev/null; true'
sig=$("$ADB" exec-out "dd if=$DEV bs=1024 skip=1 count=1 2>/dev/null" | head -c 2)
[ "$sig" = "HX" ] || [ "$sig" = "H+" ] || { echo "RESULT: NO_IOS_ROOT_ON_USERDATA (signature '$sig')"; exit 1; }
if [ $RESUME = 0 ]; then

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
fi

echo "=== $(date +%T) writing back the changed chunks"
if [ $RESUME = 0 ]; then
	hashes "$IMG" > "$WORK/after.sha1"
	paste -d' ' "$WORK/before.sha1" "$WORK/after.sha1" | awk '$1 != $2 { print NR - 1 }' > "$WORK/changed.txt"
	# runs of consecutive chunks, at most 16 MiB per write
	awk 'NR == 1 { s = $1; p = $1; next }
	     $1 == p + 1 && $1 - s < 16 { p = $1; next }
	     { print s, p - s + 1; s = $1; p = $1 }
	     END { if (NR) print s, p - s + 1 }' "$WORK/changed.txt" > "$WORK/runs.txt"
fi
echo "changed chunks: $(wc -l < "$WORK/changed.txt") MiB in $(wc -l < "$WORK/runs.txt") writes"
# adb exec-in from WSL drops the end of its input (s9io-test2: 81916 of
# 1048576 bytes arrived): push each piece as a file to TWRP's RAM /tmp,
# write it from there, read it back through exec-out (reliable).
CHUNK=$HERE/root-chunk.bin
while read -r start count; do
	dd if="$IMG" of="$CHUNK" bs=$CH skip="$start" count="$count" status=none
	want=$(sha1sum < "$CHUNK" | cut -d' ' -f1)
	"$ADB" push "$(wslpath -w "$CHUNK")" /tmp/root-chunk.bin </dev/null >/dev/null
	"$ADB" shell "/tmp/s9io w $DEV $((start * CH)) < /tmp/root-chunk.bin && rm /tmp/root-chunk.bin" </dev/null
	got=$("$ADB" exec-out "/tmp/s9io r $DEV $((start * CH)) $((count * CH))" </dev/null | sha1sum | cut -d' ' -f1)
	if [ "$want" != "$got" ]; then
		echo "RESULT: VERIFY_FAILED at MiB $start (+$count)"
		exit 1
	fi
	echo "  MiB $start +$count ok"
done < "$WORK/runs.txt"
rm -f "$CHUNK"
"$ADB" shell sync
fi
# disk extents of the installed files, for root-push-file.sh
mount -t hfsplus -o loop,ro "$IMG" "$MNT"
sed 's/\r$//' "$FILES/install.txt" | while read -r src dest mode; do
	[ -n "$src" ] && [ "${src#\#}" = "$src" ] || continue
	n=$(basename "$dest")
	# FIBMAP per 4 KiB block (filefrag only prints discontinuities on hfsplus)
	python3 -I - "$MNT$dest" > "$FILES/extents-$n.txt" <<'EOF'
import fcntl, os, struct, sys
fd = os.open(sys.argv[1], os.O_RDONLY)
bs = struct.unpack('i', fcntl.ioctl(fd, 2, b'\0' * 4))[0]          # FIGETBSZ
n = (os.fstat(fd).st_size + bs - 1) // bs
assert bs == 4096, bs
run = None
for i in range(n):
    p = struct.unpack('i', fcntl.ioctl(fd, 1, struct.pack('i', i)))[0]  # FIBMAP
    if run and p == run[2] + 1:
        run[2] = p
    else:
        if run:
            print(*run)
        run = [i, p, p]
if run:
    print(*run)
EOF
	stat -c %s "$MNT$dest" > "$FILES/size-$n.txt"
	echo "extents of $dest: $(wc -l < "$FILES/extents-$n.txt")"
	head -5 "$FILES/filefrag-$n.txt"
done
umount "$MNT"
echo "RESULT: ROOT_UPDATED"
