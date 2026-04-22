# Mini-OS 64-bit Development Record (2.1)

## Current Status: Refactored I/O and VGA Modules

### Overview
Refactored low-level I/O and VGA display interfaces into separate modules for better code organization.

### New Components

**I/O Module** (`long-mode/io.h`, `long-mode/io.c`)
- Port I/O primitives for keyboard/display communication
- `outb(port, value)`: Write byte to I/O port
- `inb(port)`: Read byte from I/O port
- Inline assembly using GCC syntax

**VGA Module** (`long-mode/vga.h`, `long-mode/vga.c`)
- Text mode display output (80x25, color)
- `update_cursor(position)`: Update hardware cursor
- `clear_screen(vga_buffer)`: Clear display with spaces
- `print_char(vga_buf, c, cursor, attr)`: Print single character
- `print_string(vga_buf, str, cursor)`: Print null-terminated string
- `print_newline(vga_buf, cursor)`: Move to next line

**Constants** (vga.h)
- `VGA_BUFFER`: 0xB8000 (text mode memory)
- `VGA_WIDTH`: 80 columns
- `VGA_HEIGHT`: 25 rows

### Updated Kernel
The kernel now includes these modules:
- Includes `io.h` and `vga.h`
- Uses `inb(0x64)` for keyboard status
- Uses `inb(0x60)` for scancode read
- Uses VGA functions for all display output

### Updated Build Commands

```bash
# Assemble boot code
as --64 boot_32.S -o output/boot_32_new.o
as --64 boot_64.S -o output/boot_64_new.o

# Compile kernel and modules
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
```

Or use the build script:
```bash
./build_64_boot.sh
```

### Date
2026-04-22