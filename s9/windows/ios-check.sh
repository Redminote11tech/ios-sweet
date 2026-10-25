# Runs on the phone in TWRP, read-only: HFS+ volume header of the iOS root
# (start of USERDATA) and whether the kernel log area holds a SWEETLOG header.
d=/dev/block/by-name/USERDATA
echo "== volume header (offset 1024)"
dd if=$d bs=1024 skip=1 count=1 2>/dev/null | od -A d -t x1 | head -8
echo "== lastMountedVersion:"
dd if=$d bs=1 skip=1032 count=4 2>/dev/null; echo
n=$(blockdev --getsize64 $d)
echo "== USERDATA bytes $n"
echo "== log header:"
dd if=$d bs=4096 skip=$((n / 4096 - 1025)) count=1 2>/dev/null | dd bs=1 count=8 2>/dev/null; echo
