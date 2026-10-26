#!/bin/bash
# Fast path after root-update.sh: overwrite an already installed file in the
# phone's iOS root in place, through the disk extents root-update.sh
# recorded (rootfiles/extents-NAME.txt). The new file must not be larger
# than the installed one (sweetgl.dylib is padded to a fixed size for that).
# Usage (WSL, run by root-push-file.bat): root-push-file.sh NAME
# where rootfiles/NAME is the new content. Phone in TWRP.
set -uo pipefail
HERE=$PWD
ADB="$HERE/adb.exe"
DEV=/dev/block/by-name/USERDATA
FILES=$HERE/rootfiles
NAME=$1
EXT=$FILES/extents-$NAME.txt
# extents not recorded yet: work them out from the last root-update image
[ -s "$EXT" ] || bash "$HERE/root-update.sh" --extents
[ -s "$EXT" ] || { echo "RESULT: NO_EXTENTS for $NAME (run root-update.bat once)"; exit 1; }
echo "$NAME: $(wc -l < "$EXT") extents"
size=$(stat -c %s "$FILES/$NAME")
max=$(cat "$FILES/size-$NAME.txt")
[ "$size" -le "$max" ] || { echo "RESULT: TOO_BIG ($size > $max bytes installed)"; exit 1; }
"$ADB" push "$(wslpath -w "$HERE/s9io")" /tmp/s9io >/dev/null || { echo "RESULT: ADB_FAILED (phone in TWRP and plugged in?)"; exit 1; }
"$ADB" shell chmod 755 /tmp/s9io
"$ADB" shell 'umount /data 2>/dev/null; true'
padded=$(mktemp)
cp "$FILES/$NAME" "$padded"
truncate -s $(( (max + 4095) / 4096 * 4096 )) "$padded"     # whole 4 KiB blocks, as on disk
# extents file: "file_block disk_start disk_end" (4 KiB blocks) per extent
CHUNK=$HERE/root-chunk.bin
total=$(wc -l < "$EXT"); done_n=0
while read -r off pstart pend; do
	n=$((pend - pstart + 1))
	dd if="$padded" of="$CHUNK" bs=4096 skip="$off" count="$n" status=none
	want=$(sha1sum < "$CHUNK" | cut -d' ' -f1)
	ok=0
	for try in 1 2 3 4; do
		if ! "$ADB" push "$(wslpath -w "$CHUNK")" /tmp/root-chunk.bin </dev/null >/dev/null 2>"$HERE/push-err.txt"; then
			echo "  extent at block $pstart: adb push failed (try $try): $(tr -d '\r' < "$HERE/push-err.txt")"
			"$ADB" wait-for-recovery </dev/null 2>/dev/null || sleep 3
			continue
		fi
		if ! "$ADB" shell "/tmp/s9io w $DEV $((pstart * 4096)) < /tmp/root-chunk.bin && rm /tmp/root-chunk.bin" </dev/null; then
			echo "  extent at block $pstart: write on the phone failed (try $try)"
			sleep 2
			continue
		fi
		got=$("$ADB" exec-out "/tmp/s9io r $DEV $((pstart * 4096)) $((n * 4096))" </dev/null | sha1sum | cut -d' ' -f1) || got=none
		if [ "$want" = "$got" ]; then
			ok=1
			break
		fi
		echo "  extent at block $pstart: read-back mismatch (try $try)"
		sleep 2
	done
	[ $ok = 1 ] || { echo "RESULT: VERIFY_FAILED at block $pstart after 4 tries"; rm -f "$padded" "$CHUNK"; exit 1; }
	echo "  extent $((++done_n))/$total ok"
done < "$EXT"
rm -f "$CHUNK"
rm -f "$padded"
"$ADB" shell sync
echo "RESULT: FILE_UPDATED $NAME ($size bytes)"
