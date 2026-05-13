# RECORD12.1 — Replace Assembly Ring-3 Stub with C Program

## Goal

Replace the hand-written assembly `syscall_user_program` in `arch/user.S` with
a C program (`ring3.c`) that uses the `output()` / `input()` syscall wrappers
from `userlib.h`.  Same behavior: print a hello, then loop reading and echoing
keystrokes via `int $0x80`.

## Motivation

`userlib.h` already provided clean C inline wrappers for `SYS_OUTPUT` and
`SYS_INPUT`, but nothing in the tree used them — the only ring-3 program was
70+ lines of assembly.  A C replacement is more readable, maintainable, and
proves the syscall interface works end-to-end from C.

## Design

### Build pipeline

The ring-3 program is not built through the `elf2minios` a.out pipeline (which
targets `PROGRAMS_BASE=0x200000` for ring-0 exec'd programs).  Instead:

1. Compile `ring3.c` → `ring3.o` (freestanding, `-O2`)
2. Link with `ring3.ld` at `0x300000` → `ring3.elf`
3. `objcopy -O binary` → `ring3.bin` (raw bytes)
4. `objcopy -I binary -O elf64-x86-64` → `ring3_bin.o` (embeddable object)
5. Link `ring3_bin.o` into the kernel — provides `_binary_ring3_bin_start` / `_end` symbols
6. Kernel copies the blob from embedded address to `0x300000` at runtime, then
   calls `enter_user_mode(0x300000, 0x400000)` — identical to the old assembly
   flow

### Linker script (`ring3.ld`)

```
ENTRY(_start)
SECTIONS {
    . = 0x300000;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) }
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
```

No kernel symbol dependencies — all I/O goes through `int $0x80`.  The program
is linked at its final runtime address so string literals resolve correctly.

## Bug: `-O0` Places `_start` After Helpers

### Symptom

Typing `user` in the shell caused the cursor to drift to a random position;
typing keys produced no echo.  System did not crash (no triple fault).

### Root cause

At `-O0`, GCC ignores `static inline`.  `output()` and `input()` become real
functions in `.text`, and the linker places them in source order:

```
0000000000300000 <output>:    ← kernel jumps here (start of binary)
  ...
0000000000300037 <input>:
  ...
000000000030006e <_start>:    ← should be the entry point, but at offset 0x6e
```

The kernel copies the binary to `0x300000` and calls
`enter_user_mode((void *)0x300000, ...)`, which jumps to the **first byte** of
the binary — `output`.  `output` runs with garbage register values from the
uninitialized user stack, executes `int $0x80` with arbitrary arguments, then
hits `leave; ret` and pops a garbage return address off the stack.  `_start`
is never reached.

### Fix

Added `-O2` to `RING3_CFLAGS`.  At `-O2` the helpers are fully inlined —
`_start` is the only function and lands at `0x300000`.  Also added
`-fcf-protection=none` to suppress the `endbr64` CET instruction that GCC
emits by default on newer toolchains.

Binary size: 303 bytes (`-O0`) → 215 bytes (`-O2`).

## Changes

### New files

| File | Purpose |
|------|---------|
| `long-mode/kernel/programs/ring3.c` | C ring-3 program using `userlib.h` |
| `long-mode/kernel/programs/ring3.ld` | Linker script for ring-3 binary |

### Modified files

| File | Change |
|------|--------|
| `long-mode/arch/user.S` | Removed `syscall_user_program` and its data (~70 lines). Kept `enter_user_mode`, `tss`, `user_program`. |
| `long-mode/kernel/kernel.c` | `extern` symbols changed from `syscall_user_program[]` / `_end` to `_binary_ring3_bin_start[]` / `_end`. Copy logic unchanged. |
| `long-mode/build_64_boot.sh` | Added ring-3 build step (compile, link, objcopy binary, embed). Added `ring3_bin.o` to kernel link list. Added `-O2 -fcf-protection=none` to `RING3_CFLAGS`. |

### `ring3.c` final form

```c
#include <stdint.h>
#include "../userlib.h"

__attribute__((noreturn))
void _start(void) {
    output(0, "hello, world from ring 3!\n", 26);

    while (1) {
        output(0, "type a key: ", 12);

        char c;
        input(1, &c, 1);

        output(0, "you typed: ", 11);
        output(0, &c, 1);
        output(0, "\n", 1);
    }
}
```

21 lines vs. the original 70 lines of assembly.  Uses the existing `userlib.h`
syscall wrappers directly — no duplicate inline asm.

## Build verification

```
$ bash long-mode/build_64_boot.sh
Kernel:    31767 bytes
Programs:  6087 bytes
ring3 binary: 215 bytes
Build complete.
Run: qemu-system-x86_64 -hda long-mode/output/disk.img
```
