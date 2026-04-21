#!/bin/bash
# build_64_boot.sh <disk_image>

set -e

DISK="$1"

if [ -z "$DISK" ]; then
    echo "Usage: ./build_stage2.sh <disk_image>"
    exit 1
fi

BUILD_DIR=./output
mkdir -p $BUILD_DIR

echo "[1] Assembling..."
as --32 boot_32.S -o $BUILD_DIR/boot_32.o
as --32 boot_64.S -o $BUILD_DIR/boot_64.o

echo "[2] Linking at 0x7E00..."
ld -m elf_i386 \
   -Ttext 0x7E00 \
   --oformat binary \
   $BUILD_DIR/boot_32.o \
   $BUILD_DIR/boot_64.o \
   -o $BUILD_DIR/stage2.bin

echo "[3] Writing to disk..."
dd if=$BUILD_DIR/stage2.bin of=$DISK \
   bs=512 seek=1 conv=notrunc

echo "✔ Done!"