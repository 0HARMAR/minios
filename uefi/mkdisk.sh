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

# Optional M0 firmware blob.  The repo never stores the binary: it is
# decompressed from the host's linux-firmware package at image-build time.
FW_SRC="${MINIOS_FW:-}"
if [ -z "$FW_SRC" ]; then
    for candidate in \
        /lib/firmware/intel/iwlwifi/iwlwifi-9000-pu-b0-jf-b0-46.ucode.zst \
        /lib/firmware/iwlwifi-9000-pu-b0-jf-b0-46.ucode.zst \
        /lib/firmware/intel/iwlwifi/iwlwifi-9000-pu-b0-jf-b0-46.ucode \
        /lib/firmware/iwlwifi-9000-pu-b0-jf-b0-46.ucode; do
        if [ -f "$candidate" ]; then
            FW_SRC="$candidate"
            break
        fi
    done
fi

FW_ARG=()
if [ -n "$FW_SRC" ] && [ -f "$FW_SRC" ]; then
    FW_BLOB="$BUILD/WIFI9.UC"
    case "$FW_SRC" in
        *.zst)
            if command -v zstd >/dev/null 2>&1; then
                zstd -dc "$FW_SRC" > "$FW_BLOB"
            elif command -v unzstd >/dev/null 2>&1; then
                unzstd -c "$FW_SRC" > "$FW_BLOB"
            else
                echo "error: zstd/unzstd is required for $FW_SRC" >&2
                exit 1
            fi
            ;;
        *)
            cp "$FW_SRC" "$FW_BLOB"
            ;;
    esac
    FW_ARG=(--firmware "$FW_BLOB")
    echo "firmware   : $FW_BLOB ($(wc -c < "$FW_BLOB") bytes) from $FW_SRC"
else
    echo "warning: no iwlwifi-9000 firmware found; image will run PCI probe only" >&2
fi

python3 "$HERE/tools/mkdisk.py" "$IMAGE" \
    "$BUILD/BOOTX64.EFI" "$BUILD/KERNEL.BIN" "${FW_ARG[@]}"

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
