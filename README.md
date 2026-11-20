# Mini-OS

A small x86-64 operating system for learning how a machine boots.

## Branch layout

This branch (`uefi-boot`) starts at the first commit and adds a native UEFI
boot path for modern notebooks.  The original BIOS boot code is kept unchanged:

- `uefi/` - UEFI loader, 64-bit kernel skeleton, GPT/FAT16 image tools
- `long-mode/` - original 64-bit BIOS boot code
- `protected-mode/` - original 32-bit BIOS boot code

## UEFI boot

```bash
./uefi/mkdisk.sh          # build BOOTX64.EFI + KERNEL.BIN and the USB image
./uefi/run_qemu.sh --usb  # test with OVMF
```

The image is a complete GPT disk with a FAT16 ESP; write it to a USB stick
with `dd` (Linux) or Rufus in DD mode (Windows), disable Secure Boot, and boot
the `UEFI:` entry.

Chinese build/run/troubleshooting guide: [uefi/README.md](uefi/README.md).

The kernel currently performs a boot self-check (long mode, CPU, memory, GOP
framebuffer) and halts.  Interrupts, keyboard and drivers are not implemented
yet.

## Original BIOS build

See the scripts in `long-mode/` and `protected-mode/`; they still produce the
1.44 MB floppy images used by the first commit.
