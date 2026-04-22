#!/bin/bash
set -e

cd "$(dirname "$0")"

mkdir -p output

# Assemble boot code
as --64 boot_32.S -o output/boot_32_new.o
as --64 boot_64.S -o output/boot_64_new.o

# Compile kernel
gcc -c -m64 -ffreestanding -fno-pie -o output/kernel.o kernel.c
gcc -c -m64 -ffreestanding -fno-pie -o output/io.o io.c
gcc -c -m64 -ffreestanding -fno-pie -o output/vga.o vga.c

# Link
ld -m elf_x86_64 -Ttext 0x7E00 output/boot_32_new.o output/boot_64_new.o output/kernel.o output/io.o output/vga.o -o output/stage2.elf

# Extract binary
objcopy -O binary output/stage2.elf output/kernel.bin

# Integrate to disk image
dd if=output/boot_mbr.bin of=output/disk.img bs=512 count=1 conv=notrunc
dd if=output/kernel.bin of=output/disk.img bs=512 seek=1 conv=notrunc

echo "Build complete: output/disk.img"