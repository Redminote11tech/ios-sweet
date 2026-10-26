#!/bin/sh
# static aarch64 Linux s9io for TWRP (no libc)
set -e
cd "$(dirname "$0")"
clang --target=aarch64-linux-gnu -O2 -ffreestanding -fno-builtin -nostdlib -static -fno-pic \
	-fno-stack-protector -mgeneral-regs-only -Wall -c s9io.c -o s9io.o
ld.lld -static -e _start s9io.o -o s9io
llvm-strip s9io
ls -la s9io
