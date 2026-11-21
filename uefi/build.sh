#!/usr/bin/env bash
#
# build.sh - build the UEFI loader and the 64-bit kernel.
#
# Outputs:
#   uefi/build/BOOTX64.EFI   PE32+ EFI application
#   uefi/build/KERNEL.BIN    flat binary linked at 0x100000
#
# Requirements: gcc, binutils (ld, objcopy, nm).  No gnu-efi or EDK2 needed.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/build"

CC="${CC:-gcc}"
LD="${LD:-ld}"
OBJCOPY="${OBJCOPY:-objcopy}"
NM="${NM:-nm}"
TRUNCATE="${TRUNCATE:-truncate}"

if [ "${1:-}" = "clean" ]; then
    if [ -n "${BUILD}" ] && [ "${BUILD}" != "/" ]; then
        rm -rf -- "${BUILD:?}"
    fi
    echo "cleaned ${BUILD}"
    exit 0
fi

CFLAGS=(
    -O2
    -ffreestanding
    -fno-stack-protector
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mgeneral-regs-only
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -fno-builtin
    -Wall
    -Wextra
)

INCLUDES=(-I"$HERE/boot" -I"$HERE/include" -I"$HERE/common" -I"$HERE/kernel")

mkdir -p "$BUILD"

compile_common() {
    local src="$1" obj="$2"
    "$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" -c "$src" -o "$obj"
}

compile_kernel() {
    local src="$1" obj="$2"
    "$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" -mcmodel=small \
        -c "$src" -o "$obj"
}

echo "[1/6] compiling shared runtime"
compile_common "$HERE/common/format.c" "$BUILD/format.o"
compile_common "$HERE/common/string.c" "$BUILD/string.o"
compile_common "$HERE/common/serial.c" "$BUILD/serial.o"

echo "[2/6] compiling UEFI loader"
"$CC" -c "$HERE/boot/handoff.S" -o "$BUILD/handoff.o"
"$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" -fshort-wchar \
    -c "$HERE/boot/bootx64.c" -o "$BUILD/bootx64.o"

echo "[3/6] linking BOOTX64.EFI"
"$LD" -mi386pep --subsystem 10 --image-base 0x10000000 -e efi_main \
    -T "$HERE/boot/boot.lds" -o "$BUILD/BOOTX64.EFI" \
    "$BUILD/bootx64.o" "$BUILD/handoff.o" \
    "$BUILD/format.o" "$BUILD/string.o" "$BUILD/serial.o"

echo "[4/6] compiling kernel"
"$CC" -c "$HERE/kernel/start.S" -o "$BUILD/start.o"
compile_kernel "$HERE/kernel/kernel.c" "$BUILD/kernel.o"
compile_kernel "$HERE/kernel/memmap.c" "$BUILD/memmap.o"
compile_kernel "$HERE/kernel/pci.c" "$BUILD/pci.o"
compile_kernel "$HERE/kernel/fw.c" "$BUILD/fw.o"
compile_kernel "$HERE/kernel/uefi_log.c" "$BUILD/uefi_log.o"

echo "[5/6] linking KERNEL.ELF"
# The flat-binary kernel has a single RWX load segment by design; there is no
# ELF program header to express per-section permissions.
"$LD" -T "$HERE/kernel/kernel.lds" --no-warn-rwx-segments \
    -o "$BUILD/KERNEL.ELF" \
    "$BUILD/start.o" "$BUILD/kernel.o" "$BUILD/memmap.o" \
    "$BUILD/pci.o" "$BUILD/fw.o" "$BUILD/uefi_log.o" \
    "$BUILD/format.o" "$BUILD/string.o" "$BUILD/serial.o"

echo "[6/6] creating KERNEL.BIN"
"$OBJCOPY" -O binary "$BUILD/KERNEL.ELF" "$BUILD/KERNEL.BIN"
KERNEL_END="$("$NM" -n "$BUILD/KERNEL.ELF" | awk '$3 == "_end" { print $1 }')"
if [ -z "$KERNEL_END" ]; then
    echo "error: _end symbol missing from KERNEL.ELF" >&2
    exit 1
fi
KERNEL_SIZE=$((16#${KERNEL_END} - 0x100000))
if [ "$KERNEL_SIZE" -le 0 ] || [ "$KERNEL_SIZE" -gt $((16 * 1024 * 1024)) ]; then
    echo "error: implausible kernel size: ${KERNEL_SIZE}" >&2
    exit 1
fi
# The flat binary must cover .bss as well: the loader copies file bytes over
# memory that firmware is not required to have zeroed.
CURRENT_SIZE="$(wc -c < "$BUILD/KERNEL.BIN")"
if [ "$CURRENT_SIZE" -lt "$KERNEL_SIZE" ]; then
    "$TRUNCATE" -s "$KERNEL_SIZE" "$BUILD/KERNEL.BIN"
fi

# Sanity-check the PE image: EFI application subsystem and base relocations.
if command -v objdump >/dev/null 2>&1; then
    if ! objdump -p "$BUILD/BOOTX64.EFI" | grep -q "EFI application"; then
        echo "error: BOOTX64.EFI is not an EFI application" >&2
        exit 1
    fi
    if ! objdump -h "$BUILD/BOOTX64.EFI" | grep -q "\.reloc"; then
        echo "error: BOOTX64.EFI has no base relocations" >&2
        exit 1
    fi
fi

printf '\nbuilt:\n'
printf '  %-48s %8s bytes\n' "$BUILD/BOOTX64.EFI" "$(wc -c < "$BUILD/BOOTX64.EFI")"
printf '  %-48s %8s bytes (link 0x100000-0x%x)\n' \
    "$BUILD/KERNEL.BIN" "$(wc -c < "$BUILD/KERNEL.BIN")" "$KERNEL_SIZE"
