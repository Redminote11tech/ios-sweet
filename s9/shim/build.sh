#!/bin/sh
# Galaxy S9 (Exynos 9810) shim. Needs clang + ld.lld + llvm-objcopy:
#   sudo apt install clang lld llvm
#
#   sh build.sh                 t0-Image (red screen test) and t2-Image (diagnostics)
#   sh build.sh payload.bin     also xnu-Image: the shim with an XNU payload linked in
#
# Then: python3 repack.py BOOT.img xnu-Image shim-boot.img
set -e
# Resolve the payload path before changing directory.
PAYLOAD="${1:+$(realpath "$1")}"
cd "$(dirname "$0")"
T="--target=aarch64-none-elf"
CFLAGS="$T -O2 -ffreestanding -fno-builtin -nostdlib -mgeneral-regs-only -mstrict-align \
  -mcmodel=tiny -fno-pic -fno-jump-tables -fno-zero-initialized-in-bss -fno-stack-protector \
  -fno-asynchronous-unwind-tables -Wall"
mkdir -p out

clang $T -c t0.S -o out/t0.o
ld.lld -Ttext=0 -e _start out/t0.o -o out/t0.elf
llvm-objcopy -O binary out/t0.elf t0-Image

clang $CFLAGS -c start.S -o out/start.o
clang $CFLAGS -c main.c -o out/main.o

link() {   # link OUT PAYLOAD_OBJ
    ld.lld -T shim.ld out/start.o out/main.o "$2" -o out/"$1".elf
    llvm-objcopy -O binary out/"$1".elf "$1"
    # pad to image_size in the header so S-Boot loads everything it is told about
    python3 -c "import struct,sys;p=sys.argv[1];d=open(p,'rb').read();n=struct.unpack_from('<Q',d,16)[0];open(p,'wb').write(d+bytes(max(0,n-len(d))))" "$1"
    echo "built $1 ($(wc -c < "$1") bytes)"
}

clang $CFLAGS -c payload.S -o out/nopayload.o
link t2-Image out/nopayload.o

# t4: GIC-400 / architected timer interrupt test
clang $CFLAGS -DGICTEST -c main.c -o out/main.o
link t4-Image out/nopayload.o

# t5: read-only UFS probe (S-Boot's request format, TUR, capacity, GPT)
clang $CFLAGS -DUFSTEST -c main.c -o out/main.o
link t5-Image out/nopayload.o

# t6: buttons + touchscreen I2C probe
clang $CFLAGS -DINPUTTEST -c main.c -o out/main.o
link t6-Image out/nopayload.o
clang $CFLAGS -c main.c -o out/main.o

if [ -n "$PAYLOAD" ]; then
    clang $CFLAGS -DPAYLOAD="\"$PAYLOAD\"" -c payload.S -o out/payload.o
    link xnu-Image out/payload.o
fi
