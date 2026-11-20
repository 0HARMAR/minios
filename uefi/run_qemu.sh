#!/usr/bin/env bash
#
# run_qemu.sh - boot the UEFI image in QEMU with OVMF.
#
#   ./uefi/run_qemu.sh                 # AHCI disk, window if a display exists
#   ./uefi/run_qemu.sh --usb           # USB mass storage (closest to a stick)
#   ./uefi/run_qemu.sh --headless      # serial on stdio, no window
#   ./uefi/run_qemu.sh --screenshot out.ppm
#
# OVMF is auto-detected under /usr/share/OVMF, /usr/share/ovmf or
# /usr/share/qemu.  The kernel halts on purpose, so a normal run keeps QEMU
# alive until Ctrl-C; --timeout N and --screenshot stop it automatically.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/build"
IMAGE="$BUILD/minios-uefi.img"

MODE=disk
HEADLESS=0
SERIAL=""
SCREENSHOT=""
TIMEOUT=0
USE_KVM=0

usage() {
    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --usb) MODE=usb ;;
        --disk) MODE=disk ;;
        --headless) HEADLESS=1 ;;
        --serial) SERIAL="${2:?--serial needs a file}"; shift ;;
        --screenshot) SCREENSHOT="${2:?--screenshot needs a file}"; shift ;;
        --timeout) TIMEOUT="${2:?--timeout needs seconds}"; shift ;;
        --kvm) USE_KVM=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

QEMU_BIN="${QEMU:-qemu-system-x86_64}"
if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    echo "error: $QEMU_BIN not found; install qemu-system-x86" >&2
    exit 1
fi

if [ ! -f "$IMAGE" ]; then
    echo "image not found, building it first..."
    "$HERE/mkdisk.sh" "$IMAGE"
fi

# Locate OVMF: prefer a CODE/VARS pair (writable NVRAM), fall back to one file.
CODE=""
VARS=""
BIOS=""
for code in \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/edk2/ovmf/OVMF_CODE.fd; do
    [ -f "$code" ] || continue
    vars="$(dirname "$code")/$(basename "${code/CODE/VARS}")"
    if [ -f "$vars" ]; then
        CODE="$code"
        VARS="$vars"
        break
    fi
done
if [ -z "$CODE" ]; then
    for bios in \
        /usr/share/ovmf/OVMF.fd \
        /usr/share/qemu/OVMF.fd \
        /usr/share/edk2/ovmf/OVMF.fd; do
        if [ -f "$bios" ]; then
            BIOS="$bios"
            break
        fi
    done
fi
if [ -z "$CODE" ] && [ -z "$BIOS" ]; then
    echo "error: OVMF firmware not found" >&2
    echo "       install ovmf (Ubuntu: sudo apt install ovmf)" >&2
    exit 1
fi

if [ -n "$CODE" ]; then
    VARS_COPY="$BUILD/OVMF_VARS.fd"
    cp "$VARS" "$VARS_COPY"
fi

QEMU_ARGS=(-machine q35 -m 1024 -no-reboot -name "Mini-OS UEFI")
if [ -n "$CODE" ]; then
    QEMU_ARGS+=(-drive "if=pflash,format=raw,readonly=on,file=$CODE")
    QEMU_ARGS+=(-drive "if=pflash,format=raw,file=$VARS_COPY")
else
    QEMU_ARGS+=(-bios "$BIOS")
fi

if [ "$MODE" = "usb" ]; then
    QEMU_ARGS+=(-device qemu-xhci,id=xhci)
    QEMU_ARGS+=(-drive "if=none,id=esp,format=raw,file=$IMAGE")
    QEMU_ARGS+=(-device usb-storage,drive=esp,bus=xhci.0)
else
    QEMU_ARGS+=(-drive "format=raw,file=$IMAGE,if=ide")
fi

DISPLAY_NONE=0
if [ "$HEADLESS" = "1" ] || [ -n "$SCREENSHOT" ]; then
    DISPLAY_NONE=1
fi
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    DISPLAY_NONE=1
fi
if [ "$DISPLAY_NONE" = "1" ]; then
    QEMU_ARGS+=(-display none)
fi
if [ "$USE_KVM" = "1" ]; then
    QEMU_ARGS+=(-accel kvm)
fi

if [ -n "$SCREENSHOT" ]; then
    # Poll the serial log until the kernel says it is halted, then dump the
    # framebuffer through the QEMU monitor.
    WAIT="${TIMEOUT:-30}"
    if [ "$WAIT" -le 0 ]; then
        WAIT=30
    fi
    SERIAL="${SERIAL:-$BUILD/serial-${MODE}.log}"
    rm -f "$SERIAL"
    # /mnt/c (DrvFs) does not support Unix sockets in WSL; keep the monitor
    # socket on the host temp filesystem.
    MON_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minios-qemu.XXXXXX")"
    MON="$MON_DIR/monitor.sock"
    SHOT="$SCREENSHOT"
    case "$SHOT" in
        *.png) SHOT_PPM="${SHOT%.png}.ppm" ;;
        *) SHOT_PPM="$SHOT" ;;
    esac
    rm -f "$SHOT" "$SHOT_PPM"

    QEMU_ARGS+=(-serial "file:$SERIAL")
    QEMU_ARGS+=(-monitor "unix:$MON,server,nowait")
    echo "booting Mini-OS ($MODE) ..."
    echo "  qemu $QEMU_BIN ${QEMU_ARGS[*]}"
    "$QEMU_BIN" "${QEMU_ARGS[@]}" &
    QPID=$!
    trap 'kill "$QPID" 2>/dev/null || true; rm -rf "$MON_DIR"' EXIT

    for ((i = 0; i < WAIT; i++)); do
        if [ -f "$SERIAL" ] && grep -q "SYSTEM HALTED" "$SERIAL" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$QPID" 2>/dev/null; then
            break
        fi
        sleep 1
    done
    sleep 1

    if ! python3 - "$MON" "$SHOT_PPM" <<'PY'
import socket
import sys
import time

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(sys.argv[1])
time.sleep(0.2)
sock.sendall(("screendump %s\n" % sys.argv[2]).encode())
time.sleep(0.5)
sock.sendall(b"quit\n")
time.sleep(0.3)
sock.close()
PY
    then
        echo "warning: could not talk to the QEMU monitor" >&2
    fi

    wait "$QPID" 2>/dev/null || true
    trap - EXIT
    rm -rf "$MON_DIR"

    if [ ! -s "$SHOT_PPM" ]; then
        echo "warning: screenshot was not produced" >&2
    elif command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -loglevel error -y -i "$SHOT_PPM" "${SHOT_PPM%.ppm}.png"
        echo "screenshot : ${SHOT_PPM%.ppm}.png"
    else
        echo "screenshot : $SHOT_PPM"
    fi
    echo "serial log : $SERIAL"
    exit 0
fi

if [ -n "$SERIAL" ]; then
    QEMU_ARGS+=(-serial "file:$SERIAL")
    echo "serial log: $SERIAL"
else
    QEMU_ARGS+=(-serial stdio)
fi

echo "booting Mini-OS ($MODE)"
echo "  $QEMU_BIN ${QEMU_ARGS[*]}"
if [ "$TIMEOUT" -gt 0 ]; then
    timeout "$TIMEOUT" "$QEMU_BIN" "${QEMU_ARGS[@]}"
else
    exec "$QEMU_BIN" "${QEMU_ARGS[@]}"
fi
