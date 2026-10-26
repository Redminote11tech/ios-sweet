#!/bin/bash
# Fast path after root-update.sh: overwrite an already installed file in the
# phone's iOS root in place, through the disk extents root-update.sh
# recorded (rootfiles/extents-NAME.txt). The new file must not be larger
# than the installed one (sweetgl.dylib is padded to a fixed size for that).
# Usage (WSL, run by root-push-file.bat): root-push-file.sh NAME
# where rootfiles/NAME is the new content. Phone in TWRP.
set -euo pipefail
HERE=$PWD
ADB="$HERE/adb.exe"
DEV=/dev/block/by-name/USERDATA
FILES=$HERE/rootfiles
NAME=$1
EXT=$FILES/extents-$NAME.txt
[ -s "$EXT" ] || { echo "RESULT: NO_EXTENTS for $NAME (run root-update.bat once)"; exit 1; }
size=$(stat -c %s "$FILES/$NAME")
max=$(cat "$FILES/size-$NAME.txt")
[ "$size" -le "$max" ] || { echo "RESULT: TOO_BIG ($size > $max bytes installed)"; exit 1; }
"$ADB" push "$(wslpath -w "$HERE/s9io")" /tmp/s9io >/dev/null
"$ADB" shell chmod 755 /tmp/s9io
"$ADB" shell 'umount /data 2>/dev/null; true'
padded=$(mktemp)
cp "$FILES/$NAME" "$padded"
truncate -s "$max" "$padded"
# extents file: "file_block disk_start disk_end" (4 KiB blocks) per extent
while read -r off pstart pend; do
	n=$((pend - pstart + 1))
	dd if="$padded" bs=4096 skip="$off" count="$n" status=none |
		"$ADB" exec-in "/tmp/s9io w $DEV $((pstart * 4096))"
	want=$(dd if="$padded" bs=4096 skip="$off" count="$n" status=none | sha1sum | cut -d' ' -f1)
	got=$("$ADB" exec-out "/tmp/s9io r $DEV $((pstart * 4096)) $((n * 4096))" </dev/null | sha1sum | cut -d' ' -f1)
	[ "$want" = "$got" ] || { echo "RESULT: VERIFY_FAILED at block $pstart"; rm -f "$padded"; exit 1; }
done < "$EXT"
rm -f "$padded"
"$ADB" shell sync
echo "RESULT: FILE_UPDATED $NAME ($size bytes)"
