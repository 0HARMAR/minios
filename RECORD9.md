# RECORD9 — a.out Executable Format

## Goal

Replace the custom 32-byte "MINI" executable header with a simplified Unix a.out
format that separates text/data/BSS, adds relocation and symbol table support,
and uses a proper ELF→a.out converter instead of `objcopy` + Python glue.

## The Format

```
+-------------------+
| exec header       |  64 bytes
+-------------------+    4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 8 + 8 + 16 = 64
| text segment      |  a_text bytes   (code + rodata)
+-------------------+
| data segment      |  a_data bytes   (initialized writable data)
+-------------------+
| text relocations  |  a_trsize bytes (0 for final executables)
+-------------------+
| data relocations  |  a_drsize bytes (0 for final executables)
+-------------------+
| symbol table      |  a_syms bytes   (0 for stripped executables)
+-------------------+
| string table      |  4-byte len + strings
+-------------------+
```

BSS is not in the file — kernel zero-fills `a_bss` bytes after data at load time.

### Header (64 bytes packed)

```
magic(4) + text(4) + data(4) + bss(4) + syms(4) + trsize(4) + drsize(4) +
version(4) + entry(8) + text_addr(8) + name(16)
```

- Magic: `0x4D4F5554` ("MOUT")
- Entry: 64-bit _start address
- text_addr: where text loads in memory (needed since programs load at varying
  addresses within `PROGRAMS_BASE`)

### Relocation entry (9 bytes) — linkable .o only

```
r_offset(4) + r_type(1) + r_sym(4)
```

- `R_ABS` (0): `*(u64*)(base+off) += symbol_value`
- `R_PC`  (1): `*(i32*)(base+off) += symbol_value - (base+off)`

### Symbol entry (13 bytes)

```
n_strx(4) + n_type(1) + n_value(8)
```

Type values match Unix a.out: N_UNDF=0, N_ABS=2, N_TEXT=4, N_DATA=6, N_BSS=8,
N_EXT=0x01 (bit flag).

## Changes

### `kernel/aout.h` — format definitions

Structs `minios_exec_t`, `minios_reloc_t`, `minios_sym_t`, macros for section
offsets (`A_TXTOFF`, `A_DATOFF`, ...), symbol types, load addresses.

### `kernel/exec.h` — simplified

Dropped old `exec_header_t`, includes `aout.h`. Interface unchanged:
`exec_init()` + `exec(name)`.

### `kernel/exec.c` — a.out loader

`exec_init()` scans concatenated a.out files at `_kernel_load_end`, copies
header+text+data to `PROGRAMS_BASE`, zeros per-program BSS, builds name→entry table.
BSS zeroing is done per-program (guess.c has 4 bytes of BSS for `rng_state`).

### `kernel/programs/program.ld` — linker script

Added `ENTRY(_start)` and `PHDRS` to create clean text(RX)/data(RW) LOAD segments:

```
PHDRS {
    text   PT_LOAD FLAGS(5);   /* RX */
    data   PT_LOAD FLAGS(6);   /* RW */
}
```

This lets `elf2minios` cleanly separate text and data via program headers.

### `tools/elf2minios.c` — ELF→a.out converter (already existed)

Reads ELF64 PHDRs, extracts text(RX) and data(RW) LOAD segments, writes a.out.
BSS = data.memsz − data.filesz. Accepts only `ET_EXEC` x86_64 ELFs.

### `build_64_boot.sh` — updated pipeline

- Builds `elf2minios` from `tools/elf2minios.c`
- `build_program()`: `gcc -c` → `ld -T program.ld` → `elf2minios elf aout name`
- Header size changed from 32 → 64 bytes (`HDR_SZ=64`)
- Terminator: 64 bytes of zeros (was 32 bytes)

## Build verification

All 7 programs converted and concatenated correctly:

```
hello   text=181   entry=0x100040
echo    text=474   entry=0x100135
primes  text=821   entry=0x10046F
fib     text=646   entry=0x1007E4
chars   text=645   entry=0x100A15
calc    text=1582  entry=0x100F48
guess   text=1226  bss=4  entry=0x10146D
```

Programs are loaded at the same addresses they were linked at (build script and
loader use the same sequential layout). Text+data are contiguous (OMAGIC
semantics — no page protection yet).

## File summary

| File | Action |
|------|--------|
| `long-mode/kernel/aout.h` | Existed — format structs, macros, constants |
| `long-mode/kernel/exec.h` | Rewritten — includes aout.h, drops old MINI struct |
| `long-mode/kernel/exec.c` | Rewritten — a.out parser + BSS zeroing |
| `long-mode/kernel/programs/program.ld` | Added `ENTRY(_start)` + `PHDRS` |
| `tools/elf2minios.c` | Existed — ELF64→a.out converter |
| `long-mode/build_64_boot.sh` | Updated to use elf2minios; HDR_SZ 32→64 |
| `DESIGN-aout-format.md` | Full design document |
