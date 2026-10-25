# Runs on the phone in TWRP: prints the last 1025 x 4 KiB blocks of USERDATA
# (the kernel's persistent console log, written with ufsrw=1). Read-only.
# tail, not dd: TWRP's shell arithmetic and BusyBox 1.22 dd skip are 32-bit,
# so dd skip=<end of a 54 GB partition> reads the wrong place.
tail -c 4198400 /dev/block/by-name/USERDATA
