# RECORD12.2 — Ring-3 a.out Loading via exec_user + BSS Overlap Bug

## Goal

Replace the raw binary embedding of the ring-3 program with a proper a.out
executable that is stored in `programs.bin`, loaded by a new `exec_user()`
function, and entered via `enter_user_mode`.  This makes ring-3 program loading
follow the same pattern as ring-0 `exec()` — read header, load segments, jump
to entry point — except `exec_user` loads to the program's linked address
(0x300000) and drops to ring 3 instead of calling the entry point directly.

## Background

[RECORD12.1](RECORD12.1.md) replaced the assembly `syscall_user_program` with a
C `ring3.c`, but the binary was embedded raw into the kernel via
`objcopy -I binary`.  The `user` shell command copied the raw bytes to 0x300000
and called `enter_user_mode`.  This was a hard-coded, non-standard loading path.

A real OS stores userland programs as executable files on disk, parses headers,
loads segments to the correct addresses, and then transitions to user mode.  We
already have the a.out format (`elf2minios`), program loading (`exec_init`), and
execution (`exec`) for ring-0 programs — this work adds the ring-3 equivalent.

## Design

### Build: ring3 through elf2minios

`ring3.c` is compiled and linked into an ELF at 0x300000 (using `ring3.ld`),
then converted to a.out format by `elf2minios`.  The resulting `ring3.aout` is
appended to `programs.bin` alongside the 7 ring-0 programs.

`ring3.ld` must declare explicit `PHDRS` so elf2minios sees proper `PT_LOAD`
segments:

```
PHDRS {
    text   PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data   PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}
SECTIONS {
    . = 0x300000;
    .text   : { *(.text*) }   :text
    .rodata : { *(.rodata*) } :text
    .data   : { *(.data*) }   :data
    .bss    : { *(.bss*) }    :data
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
```

No `--defsym` for kernel symbols — all I/O goes through `int $0x80`.

### exec_user: load to linked address, drop to ring 3

A new function `exec_user(name)` in `exec.c`:

1. Find the program by name in `prog_table`
2. Read the `minios_exec_t` header from `PROGRAMS_BASE + prog_table[i].offset`
3. Copy `a_text` bytes to `a_text_addr` (0x300000 for ring3)
4. Copy `a_data` bytes to `a_text_addr + a_text`
5. Zero-fill `a_bss` bytes at `a_text_addr + a_text + a_data`
6. Call `enter_user_mode(a_entry, 0x400000)`

To support this, `prog_table` gained a `uint32_t offset` field recording each
program's byte offset within `PROGRAMS_BASE`, so `exec_user` can find the a.out
header.

### Kernel shell: user command

The `user` shell command now calls `exec_user("ring3")` instead of manually
copying bytes and calling `enter_user_mode`.  The old `_binary_ring3_bin_start`
/ `_end` externs and the copy loop are removed.

## Bug: BSS Zeroing Corrupts Next Program's Header

### Symptom

`hello` worked, but `user` printed "ring3 not found".

### Root cause

`exec_init` Phase 3 iterates through programs at `PROGRAMS_BASE`, and for each
program zeroes its BSS at `PROGRAMS_BASE + offset + HDRSIZE + text + data`.
Programs are packed sequentially — the BSS area of program N occupies the same
memory as the header of program N+1.

| Program | a_bss | BSS zero range (at PROGRAMS_BASE) |
|---------|-------|-----------------------------------|
| ...     | ...   | ...                               |
| guess   | 4     | 0x200000 + 4733 + 64 + 1226 = **0x201787** |
| ring3   | 0     | —                                 |
| terminator | —  | starts at 0x20189E                |

Ring3's header was at `0x201787` — exactly where guess's 4-byte BSS was zeroed.
Phase 3 zeroed guess's BSS (4 bytes of zeros at 0x201787), which overwrote
ring3's `a_magic` from `0x4D4F5554` to `0x00000000`.  When the loop reached
ring3, it saw `magic=0` and broke — ring3 was never added to `prog_table`.

Before ring3 was added, guess's BSS overlapped the terminator (already zeros),
so the bug was latent.

```
Phase 3 processing:

 off=4733: guess
   zero BSS at 0x201787 (4 bytes) → writes 0x00000000 over ring3's a_magic
   off += 64 + 1226 + 0 = 6023

 off=6023: read magic at 0x201787 → 0x00000000 ≠ AOUT_MAGIC → BREAK
   ring3 never added to prog_table!
```

### Fix

Move `ring3` before `guess` in `programs.bin`.  Ring3 has `a_bss=0`, so its
zeroing is a no-op.  Guess now follows ring3, and its BSS zeroing overlaps the
terminator (already zero).

```
Fixed layout:

 off=4733: ring3 (a_bss=0, no-op)
   off += 64 + 215 + 0 = 5012

 off=5012: guess
   zero BSS at 0x20189F (4 bytes) → writes over terminator (already zero)
   off += 64 + 1227 + 0 = 6303

 off=6303: terminator → magic=0 → BREAK
   All 8 programs processed.
```

## Changes

### New files

| File | Purpose |
|------|---------|
| `long-mode/kernel/programs/ring3.ld` | Linker script with explicit PHDRS for elf2minios |

### Modified files

| File | Change |
|------|--------|
| `long-mode/kernel/programs/ring3.c` | Uses `#include "../userlib.h"` for syscall wrappers |
| `long-mode/kernel/exec.h` | Added `exec_user()` declaration |
| `long-mode/kernel/exec.c` | Added `offset` field to prog_table entries; added `exec_user()` — loads segments to linked address, calls `enter_user_mode` |
| `long-mode/kernel/kernel.c` | `user` command calls `exec_user("ring3")`; removed old `_binary_ring3_bin_*` externs and copy loop |
| `long-mode/build_64_boot.sh` | Ring3 built through elf2minios and appended to programs.bin (before guess). Removed raw binary embedding and `ring3_bin.o` from kernel link. |
| `long-mode/arch/user.S` | Removed `syscall_user_program` and its data (~70 lines). Kept `enter_user_mode`, `tss`, `user_program`. |

### Build order fix

In `build_64_boot.sh`, ring3 is now built before guess:

```
hello → echo → primes → fib → chars → calc → ring3 → guess → terminator
```

This ensures guess's `a_bss=4` zeroing hits the terminator (harmless) instead of
ring3's header.

## Build verification

```
$ bash long-mode/build_64_boot.sh
Kernel:    31552 bytes
Programs:  6367 bytes  (8 programs + terminator)
ring3:     279 bytes   (64 header + 215 text)
Build complete.
Run: qemu-system-x86_64 -hda long-mode/output/disk.img
```

Type `user` — prints "hello, world from ring 3!", then the echo loop works.
