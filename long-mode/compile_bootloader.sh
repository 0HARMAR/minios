#!/bin/bash
# compile_bootloader.sh <source> <disk> [sectors]

src="$1"
disk="$2"
sectors="${3:-55}"

name="${src##*/}"
object="${name%.*}.o"
bin="boot_mbr.bin"

mkdir -p ./output

as --32 --defsym "SECTORS=$sectors" "$src" -o ./output/"$object"
ld -m elf_i386 -Ttext 0x7c00 --oformat binary ./output/"$object" -o ./output/"$bin"
dd if=./output/"$bin" of="$disk" bs=512 count=1 conv=notrunc
