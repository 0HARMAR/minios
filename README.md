# Mini-OS

A minimal operating system project demonstrating both protected mode and long mode booting.

## Structure

- `long-mode/` - 64-bit long mode kernel
- `protected-mode/` - 32-bit protected mode kernel

## Quick Start

Create a floppy disk image first:

```bash
# Create a 1.44MB floppy image
dd if=/dev/zero of=floppy.img bs=512 count=2880
```

Then build either protected mode or long mode:

---

## Protected Mode (32-bit)

Runs in 32-bit protected mode - the traditional 32-bit mode before x86-64.

### Build

```bash
cd protected-mode

# Step 1: Compile the bootloader
./compile_bootloader.sh boot_loader.S ../floppy.img

# Step 2: Build and write the kernel
./build_kernel.sh ../floppy.img
```

### How it works

1. BIOS loads `boot_loader.S` at 0x7C00 (boot sector)
2. Bootloader loads kernel from sector 2+ into 0x7E00
3. `boot_32.S` runs in 32-bit protected mode
4. `kernel.c` is the minimal C kernel

---

## Long Mode (64-bit)

Runs in 64-bit long mode (x86-64).

### Build

```bash
cd long-mode

# Step 1: Compile the bootloader
./compile_bootloader.sh boot_loader.S ../floppy.img

# Step 2: Build and write the 64-bit boot sector
./build_64_boot.sh ../floppy.img
```

### How it works

1. BIOS loads `boot_loader.S` at 0x7C00 (boot sector)
2. Bootloader loads stage 2 from sector 2+ into 0x7E00
3. `boot_32.S` sets up 32-bit protected mode
4. `boot_64.S` enables PAE and long mode, switches to 64-bit
5. Control transfers to 64-bit kernel code

---

## Running

Use an emulator like QEMU:

```bash
qemu-system-i386 -drive format=raw,file=floppy.img
```

For long mode (64-bit):
```bash
qemu-system-x86_64 -drive format=raw,file=floppy.img
```

---

## Requirements

- `as` (GNU assembler)
- `ld` (GNU linker)
- `gcc` (for protected mode kernel)
- `dd` (coreutils)
- `qemu` (for testing)