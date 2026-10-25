# Runs on the phone in TWRP: prints the last 1025 x 4 KiB blocks of USERDATA
# (the kernel's persistent console log, written with ufsrw=1). Read-only.
d=/dev/block/by-name/USERDATA
n=$(blockdev --getsize64 $d)
dd if=$d bs=4096 skip=$((n / 4096 - 1025)) count=1025 2>/dev/null
