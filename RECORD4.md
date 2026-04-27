# RECORD4: First Kernel-Space Executable & Global VGA Cursor

## Summary

Two milestones in this session:

1. **First executable runs in kernel space** — the `hello` program is compiled separately, packed with a MINI exec header, loaded from disk, and called as a function from the shell via `exec()`.

2. **VGA cursor refactored to a global** — the shell's cursor position is now shared with user programs through a global variable, allowing programs to write output at the current cursor position rather than hardcoded screen locations.

## Architecture

### Program execution flow

```
disk ──► bootloader ──► kernel.bin + programs.bin loaded contiguously at 0x7E00
                           │
                     exec_init() copies programs from _kernel_load_end
                           │         to PROGRAMS_BASE (0x100000)
                           ▼
                     prog_table[0].entry = _start @ 0x100020
                           │
                     shell types "hello" ──► exec("hello")
                           │                    │
                           ▼                    ▼
                     prog_table[i].entry() ── calls _start()
                                                    │
                                           prints output via kernel
                                           functions (print_string, etc.)
```

### MINI exec header format (32 bytes, packed)

| Offset | Size | Field      |
|--------|------|------------|
| 0      | 4    | magic (0x4D494E49) |
| 4      | 16   | name       |
| 20     | 4    | code_size  |
| 24     | 8    | entry (absolute address) |

### Shared VGA cursor

```c
// kernel/kernel.c — placed in .data to avoid BSS overlap
__attribute__((section(".data"))) uint32_t vga_cursor_pos;

// kernel/programs/hello.c — reads and writes the global
extern uint32_t vga_cursor_pos;
```

Programs link against kernel symbols via `--defsym` in the build script. The linker resolves `vga_cursor_pos`, `print_string`, `print_newline`, and `update_cursor` to their actual addresses in the kernel ELF.

## Bug: BSS overlap between vga_cursor_pos and programs.bin

### Symptom

First `hello` execution worked. Subsequent executions caused a reboot (triple fault).

### Root cause

`vga_cursor_pos` was an uninitialized global placed in `.bss` at address 0xD840. `programs.bin` is loaded at `_kernel_load_end` (0xD822), extending into `.bss`. So `vga_cursor_pos` physically overlapped with the programs.bin data.

Before `exec_init()` copied programs from `_kernel_load_end` to `PROGRAMS_BASE`, the shell had already set `vga_cursor_pos = 320`. The copy loop read this value instead of the original program bytes, corrupting the entry point at the destination.

```
                _kernel_load_end
                    │
  ┌─────────────────┤
  │  .data          │  .bss (also where programs.bin lands)
  │                 │
  │          vga_cursor_pos (offset 0x1E into programs.bin)
  │                 │  ├── header ──┤ ├── code ──┤
  │                 │  [magic|name|sz|entry...]
  ▼                 ▼
 exec_init() copies corrupted entry bytes → jump to garbage address → #PF → triple fault
```

### Fix

`__attribute__((section(".data")))` places `vga_cursor_pos` in `.data` (before `_kernel_load_end`), which is:
- outside the programs.bin memory region (no overlap with source data during copy)
- not zeroed by `exec_init()` Phase 2 (only `.bss` is cleared)
- included in kernel.bin (persisted on disk)

## VGA improvements

- **Scroll support**: `scroll_screen()` shifts rows 1..24 up and blanks row 24. `print_newline()` auto-scrolls when the cursor reaches the last line (row 24).
- **Shared cursor**: Programs read `vga_cursor_pos` to start writing at the shell's current cursor position instead of hardcoded coordinates. They write the final position back so the shell prompt follows.

## Files changed

| File | Change |
|------|--------|
| `kernel/exec.c` | Program loader: copy from BSS-overlap region, build name→entry table |
| `kernel/exec.h` | MINI header struct, PROGRAMS_BASE, API declarations |
| `kernel/kernel.c` | Global `vga_cursor_pos` in `.data`, shell loop uses it |
| `kernel/programs/hello.c` | Reads/writes `vga_cursor_pos`, calls `update_cursor()` |
| `kernel/link.ld` | Kernel linker script (0x7E00 base) |
| `kernel/programs/program.ld` | Program linker script (linked at load address) |
| `drivers/vga.c` | Added `scroll_screen()`, `print_newline` auto-scrolls |
| `drivers/vga.h` | Declared `scroll_screen()` |
| `build_64_boot.sh` | Extracts `vga_cursor_pos` from kernel ELF, passes via `--defsym` |
