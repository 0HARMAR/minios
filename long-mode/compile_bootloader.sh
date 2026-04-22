#!/bin/bash
# compile_bootloader.sh <source> <disk>

src="$1"
object="${src%.*}.o"
bin="boot_mbr.bin"

disk="$2"

mkdir -p ./output

as --32 $src -o ./output/$object
ld -m elf_i386 -Ttext 0x7c00 --oformat binary ./output/$object -o ./output/$bin
dd if=./output/$bin of=$disk bs=512 count=1 conv=notrunc