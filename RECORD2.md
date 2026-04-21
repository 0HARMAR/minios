# Mini-OS 64-bit Development Record

## Current Status: Working 64-bit Echo Shell

### Overview
Successfully boot to 64-bit long mode with a simple C-based echo shell.

### Components

**Bootloader** (`long-mode/boot_loader.S`)
- Sector 0: 16-bit real mode, loads kernel from disk
- Reads 45 sectors (23040 bytes) to address 0x7E00

**Boot Code** (`long-mode/boot_32.S`, `long-mode/boot_64.S`)
- `boot_32.S`: 32-bit entry, enables A20 line, switches to protected mode
- `boot_64.S`: Sets up long mode
  - CPUID check for long mode support
  - PML4/PDP/PD page tables (1GB identity mapping via 2MB huge pages)
  - Enables PAE, loads CR3, sets EFER.LME, enables paging
  - Loads 64-bit GDT, jumps to 64-bit mode
  - Debug: prints "64-bit OK!" to VGA before kernel

**Kernel** (`long-mode/kernel.c`)
- Simple echo shell in 64-bit mode
- VGA text output at 0xB8000
- PS/2 keyboard input (polling port 0x60/0x64)
- Scan code to ASCII mapping
- Command buffer with echo on Enter

### Build Commands

```bash
# Assemble boot code
as --64 boot_32.S -o output/boot_32_new.o
as --64 boot_64.S -o output/boot_64_new.o

# Compile kernel
gcc -c -m64 -ffreestanding -fno-pie -o output/kernel.o kernel.c

# Link
ld -m elf_x86_64 -Ttext 0x7E00 output/boot_32_new.o output/boot_64_new.o output/kernel.o -o output/stage2.elf

# Extract binary
objcopy -O binary output/stage2.elf output/kernel.bin

# Integrate to disk image
dd if=output/boot_mbr.bin of=output/disk.img bs=512 count=1 conv=notrunc
dd if=output/kernel.bin of=output/disk.img bs=512 seek=1 conv=notrunc
```

### Disk Layout
- LBA 0 (sector 0): boot_loader.S (512 bytes)
- LBA 1-45 (sector 1-45): kernel.bin (~21KB)

### Testing
```bash
qemu-system-x86_64 -drive format=raw,file=output/disk.img -display curses
# or
qemu-system-x86_64 -drive format=raw,file=output/disk.img -monitor stdio
```

### Date
2026-04-21