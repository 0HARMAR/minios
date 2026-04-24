#!/bin/bash
set -e

cd "$(dirname "$0")"
BASE_DIR=$(pwd)

mkdir -p output

# Assemble boot code
as --64 boot_32.S -o output/boot_32_new.o
as --64 boot_64.S -o output/boot_64_new.o

# Assemble ISR
as --64 arch/isr.S -o output/isr.o

# Compile kernel
gcc -c -m64 -ffreestanding -fno-pie -I"$BASE_DIR/arch" -I"$BASE_DIR/drivers" -I"$BASE_DIR/io" -I"$BASE_DIR/kernel" -o output/kernel.o kernel/kernel.c
gcc -c -m64 -ffreestanding -fno-pie -I"$BASE_DIR/arch" -I"$BASE_DIR/drivers" -I"$BASE_DIR/io" -I"$BASE_DIR/kernel" -o output/io.o io/io.c
gcc -c -m64 -ffreestanding -fno-pie -I"$BASE_DIR/arch" -I"$BASE_DIR/drivers" -I"$BASE_DIR/io" -I"$BASE_DIR/kernel" -o output/vga.o drivers/vga.c
gcc -c -m64 -ffreestanding -fno-pie -I"$BASE_DIR/arch" -I"$BASE_DIR/drivers" -I"$BASE_DIR/io" -I"$BASE_DIR/kernel" -o output/pic.o arch/pic.c
gcc -c -m64 -ffreestanding -fno-pie -I"$BASE_DIR/arch" -I"$BASE_DIR/drivers" -I"$BASE_DIR/io" -I"$BASE_DIR/kernel" -o output/idt.o arch/idt.c
gcc -c -m64 -ffreestanding -fno-pie -I"$BASE_DIR/arch" -I"$BASE_DIR/drivers" -I"$BASE_DIR/io" -I"$BASE_DIR/kernel" -o output/irq.o arch/irq.c
gcc -c -m64 -ffreestanding -fno-pie -I"$BASE_DIR/arch" -I"$BASE_DIR/drivers" -I"$BASE_DIR/io" -I"$BASE_DIR/kernel" -o output/keyboard.o drivers/keyboard.c
gcc -c -m64 -ffreestanding -fno-pie -I"$BASE_DIR/arch" -I"$BASE_DIR/drivers" -I"$BASE_DIR/io" -I"$BASE_DIR/kernel" -o output/interrupts.o kernel/interrupts.c

# Link
ld -m elf_x86_64 -Ttext 0x7E00 output/boot_32_new.o output/boot_64_new.o \
    output/kernel.o output/io.o output/vga.o output/isr.o \
    output/pic.o output/idt.o output/irq.o output/keyboard.o output/interrupts.o \
    -o output/stage2.elf

# Extract binary
objcopy -O binary output/stage2.elf output/kernel.bin

# Integrate to disk image
dd if=output/boot_mbr.bin of=output/disk.img bs=512 count=1 conv=notrunc
dd if=output/kernel.bin of=output/disk.img bs=512 seek=1 conv=notrunc

echo "Build complete: output/disk.img"