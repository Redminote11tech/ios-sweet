# Runs on the phone in TWRP, read-only. Writes to /tmp only.
d=/dev/block/by-name/USERDATA
n=$(blockdev --getsize64 $d)
echo "link: $(readlink -f $d)  bytes: $n  blocks: $((n / 4096))"
grep -E "sda$|sda25|$(basename $(readlink -f $d))" /proc/partitions
which dd; dd --help 2>&1 | head -2
# A: last block of the 16 GiB iOS volume (alternate volume header "HX" at +3072)
dd if=$d bs=4096 skip=4194303 count=1 of=/tmp/a.bin 2>/dev/null
# B: last 2 blocks of USERDATA, bs 4096 (where probe #8 wrote "S9WRTEST")
dd if=$d bs=4096 skip=$((n / 4096 - 2)) count=2 of=/tmp/b.bin 2>/dev/null
# C: last 1 MiB of USERDATA, bs 1 MiB
dd if=$d bs=1048576 skip=$((n / 1048576 - 1)) count=1 of=/tmp/c.bin 2>/dev/null
# D: same last 8 KiB through tail
tail -c 8192 $d > /tmp/d.bin
ls -l /tmp/a.bin /tmp/b.bin /tmp/c.bin /tmp/d.bin
