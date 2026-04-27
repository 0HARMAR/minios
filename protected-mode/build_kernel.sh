#!/bin/bash
# build_kernel.sh <disk_image>

set -e

DISK="$1"

if [ -z "$DISK" ]; then
    echo "Usage: ./build_kernel.sh <disk_image>"
    exit 1
fi

BUILD_DIR=./output
mkdir -p $BUILD_DIR

echo "[1] Assembling boot_32.S..."
as --32 boot_32.S -o $BUILD_DIR/boot_32.o

echo "[2] Compiling kernel.c..."
gcc -m32 -ffreestanding -fno-pic -fno-stack-protector \
    -nostdlib -nostartfiles -nodefaultlibs \
    -c kernel.c -o $BUILD_DIR/kernel.o

echo "[3] Linking kernel..."
ld -m elf_i386 \
   -Ttext 0x7E00 \
   --oformat binary \
   $BUILD_DIR/boot_32.o \
   $BUILD_DIR/kernel.o \
   -o $BUILD_DIR/kernel.bin

echo "[4] Writing kernel to disk..."
# write from sector 2 (bootloader occupies sector 1)
dd if=$BUILD_DIR/kernel.bin of=$DISK \
   bs=512 seek=1 conv=notrunc

echo "✔ Kernel written successfully!"