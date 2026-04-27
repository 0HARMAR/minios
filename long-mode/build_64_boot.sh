#!/bin/bash
set -e

cd "$(dirname "$0")"
BASE_DIR=$(pwd)
OUTDIR="$BASE_DIR/output"
PROG_DIR="$BASE_DIR/kernel/programs"

mkdir -p "$OUTDIR"

# ============================================================
# 1. Assemble boot stages
# ============================================================
as --64 boot_32.S -o "$OUTDIR/boot_32_new.o"
as --64 boot_64.S -o "$OUTDIR/boot_64_new.o"
as --64 arch/isr.S  -o "$OUTDIR/isr.o"

# ============================================================
# 2. Compile kernel C files
# ============================================================
CFLAGS="-c -m64 -ffreestanding -fno-pie -mcmodel=small"
CFLAGS="$CFLAGS -I$BASE_DIR/arch -I$BASE_DIR/drivers -I$BASE_DIR/io -I$BASE_DIR/kernel"

gcc $CFLAGS -o "$OUTDIR/kernel.o"     kernel/kernel.c
gcc $CFLAGS -o "$OUTDIR/io.o"         io/io.c
gcc $CFLAGS -o "$OUTDIR/vga.o"        drivers/vga.c
gcc $CFLAGS -o "$OUTDIR/pic.o"        arch/pic.c
gcc $CFLAGS -o "$OUTDIR/idt.o"        arch/idt.c
gcc $CFLAGS -o "$OUTDIR/irq.o"        arch/irq.c
gcc $CFLAGS -o "$OUTDIR/keyboard.o"   drivers/keyboard.c
gcc $CFLAGS -o "$OUTDIR/interrupts.o" kernel/interrupts.c
gcc $CFLAGS -o "$OUTDIR/exec.o"       kernel/exec.c

# ============================================================
# 3. Link kernel
# ============================================================
KERNEL_OBJS="$OUTDIR/boot_32_new.o $OUTDIR/boot_64_new.o \
    $OUTDIR/kernel.o $OUTDIR/io.o $OUTDIR/vga.o $OUTDIR/isr.o \
    $OUTDIR/pic.o $OUTDIR/idt.o $OUTDIR/irq.o $OUTDIR/keyboard.o \
    $OUTDIR/interrupts.o $OUTDIR/exec.o"

ld -m elf_x86_64 -T kernel/link.ld $KERNEL_OBJS -o "$OUTDIR/stage2.elf"
objcopy -O binary "$OUTDIR/stage2.elf" "$OUTDIR/kernel.bin"
echo "Kernel: $OUTDIR/kernel.bin ($(stat -c %s "$OUTDIR/kernel.bin") bytes)"

# ============================================================
# 4. Extract kernel symbols for programs
# ============================================================
sym_addr() { nm "$OUTDIR/stage2.elf" | grep " $1\$" | awk '{print "0x"$1}'; }

P_STR=$(sym_addr print_string)
P_NL=$(sym_addr print_newline)
P_CH=$(sym_addr print_char)
P_CLR=$(sym_addr clear_screen)
P_UC=$(sym_addr update_cursor)
P_VGA=$(sym_addr vga_cursor_pos)

echo "Kernel symbols:"
echo "  print_string    = $P_STR"
echo "  print_newline   = $P_NL"
echo "  print_char      = $P_CH"
echo "  clear_screen    = $P_CLR"
echo "  update_cursor   = $P_UC"
echo "  vga_cursor_pos  = $P_VGA"

# ============================================================
# 5. Build programs -> programs.bin
# ============================================================
PROGRAMS_BASE=0x100000
HDR_SZ=32
offset=0

rm -f "$OUTDIR/programs.bin"

build_program() {
    local name="$1"
    local src="$PROG_DIR/${name}.c"
    local obj="$OUTDIR/${name}.o"
    local elf="$OUTDIR/${name}.elf"
    local bin="$OUTDIR/${name}.bin"
    local code_addr=$(( PROGRAMS_BASE + offset + HDR_SZ ))

    echo "Building: $name  (header=0x$(printf '%X' $((PROGRAMS_BASE+offset)))  code=0x$(printf '%X' $code_addr))"

    gcc -c -m64 -ffreestanding -fno-pie -mcmodel=small -o "$obj" "$src"

    local ld_script="$PROG_DIR/program.ld"
    ld -m elf_x86_64 -T "$ld_script" \
        --defsym=BASE=$(printf '0x%X' $code_addr) \
        --defsym=print_string=$P_STR \
        --defsym=print_newline=$P_NL \
        --defsym=print_char=$P_CH \
        --defsym=clear_screen=$P_CLR \
        --defsym=update_cursor=$P_UC \
        --defsym=vga_cursor_pos=$P_VGA \
        "$obj" -o "$elf"

    objcopy -O binary "$elf" "$bin"
    local code_sz=$(stat -c %s "$bin")
    local entry=$(nm "$elf" | grep " _start\$" | awk '{print "0x"$1}')

    if [ -z "$entry" ]; then
        echo "ERROR: no _start symbol in $name"
        exit 1
    fi

    echo "  code_size=$code_sz  entry=$entry"

    # 32-byte header: magic(u32) + name[16] + code_size(u32) + entry(u64)
    python3 -c "
import struct, sys
h = struct.pack('<I16sIQ', 0x4D494E49, b'${name}\x00'.ljust(16,b'\x00')[:16], $code_sz, $entry)
sys.stdout.buffer.write(h)
" > "$OUTDIR/${name}_hdr.bin"

    cat "$OUTDIR/${name}_hdr.bin" "$bin" >> "$OUTDIR/programs.bin"
    offset=$(( offset + HDR_SZ + code_sz ))
}

build_program "hello"

# Terminator header (magic=0)
python3 -c "
import struct, sys
sys.stdout.buffer.write(struct.pack('<I16sIQ', 0, b'\x00'*16, 0, 0))
" >> "$OUTDIR/programs.bin"

echo "Programs: $OUTDIR/programs.bin ($(stat -c %s "$OUTDIR/programs.bin") bytes)"

# ============================================================
# 6. Write disk image as one continuous blob
# ============================================================
# Concatenate kernel + programs with zero gap so programs land
# at _kernel_load_end in memory (no sector-alignment gap).
cat "$OUTDIR/kernel.bin" "$OUTDIR/programs.bin" > "$OUTDIR/payload.bin"

total_bytes=$(stat -c %s "$OUTDIR/payload.bin")
total_sectors=$(( (total_bytes + 511) / 512 + 1 ))  # +1 for boot sector

echo "Total sectors to load: $total_sectors (payload: $total_bytes bytes)"

# Build bootloader with correct sector count
bash "$BASE_DIR/compile_bootloader.sh" "$BASE_DIR/boot_loader.S" "$OUTDIR/disk.img" "$total_sectors"

# Write payload starting at sector 1
dd if="$OUTDIR/payload.bin" of="$OUTDIR/disk.img" bs=512 seek=1 conv=notrunc status=none

echo "============================================"
echo "Build complete: $OUTDIR/disk.img"
echo "Kernel:   $(stat -c %s "$OUTDIR/kernel.bin") bytes"
echo "Programs: $(stat -c %s "$OUTDIR/programs.bin") bytes"
echo "Payload:  $total_bytes bytes contiguously @ sector 1"
echo "============================================"
