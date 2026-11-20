#!/usr/bin/env bash
#
# mkdisk.sh - build Mini-OS and create the bootable GPT/FAT16 USB image.
#
#   ./uefi/mkdisk.sh [OUTPUT.img]
#
# The image is a complete GPT disk, not a bare filesystem: write it to the
# whole USB device (dd/Rufus), never to a partition.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/build"
IMAGE="${1:-$BUILD/minios-uefi.img}"

if [ "${MINIOS_SKIP_BUILD:-0}" != "1" ]; then
    "$HERE/build.sh"
fi

for f in "$BUILD/BOOTX64.EFI" "$BUILD/KERNEL.BIN"; do
    if [ ! -f "$f" ]; then
        echo "error: $f is missing; run uefi/build.sh first" >&2
        exit 1
    fi
done

python3 "$HERE/tools/mkdisk.py" "$IMAGE" \
    "$BUILD/BOOTX64.EFI" "$BUILD/KERNEL.BIN"

# Optional integrity checks when the host tools are available.
if command -v sgdisk >/dev/null 2>&1; then
    sgdisk -v "$IMAGE" >/dev/null
    echo "GPT check  : sgdisk -v OK"
fi

if command -v fsck.vfat >/dev/null 2>&1 && command -v sgdisk >/dev/null 2>&1; then
    START="$(sgdisk -i 1 "$IMAGE" | awk '/First sector/ { print $3 }')"
    END="$(sgdisk -i 1 "$IMAGE" | awk '/Last sector/ { print $3 }')"
    if [ -n "$START" ] && [ -n "$END" ]; then
        TMP="$(mktemp)"
        trap 'rm -f "$TMP"' EXIT
        dd if="$IMAGE" of="$TMP" bs=512 skip="$START" \
            count=$((END - START + 1)) status=none
        fsck.vfat -n "$TMP" >/dev/null
        echo "FAT check  : fsck.vfat OK"
    fi
fi

cat <<EOF

image ready: $IMAGE

Write it to a USB stick (this erases the whole stick):
  Linux : sudo dd if=$IMAGE of=/dev/sdX bs=4M status=progress conv=fsync
  Windows: use Rufus in "DD mode", or Win32DiskImager

Then on the notebook:
  1. disable Secure Boot in the firmware setup,
  2. boot the UEFI entry for the stick (usually F12/F9/F10/Esc),
  3. the screen should show "Mini-OS UEFI Loader" and then the kernel log.

Details: $HERE/README.md
EOF
