# RECORD5 — New User Programs & Bug Fixes

## New Programs Added

Six new user-space programs under `long-mode/kernel/programs/`:

| Program | Source | Description |
|---------|--------|-------------|
| `echo` | `echo.c` | Interactive line-echo utility. Reads keyboard input, supports backspace, echoes each line prefixed with `=>`. Exits on empty line. |
| `primes` | `primes.c` | Prime number sieve up to 500. Prints primes in 10-column layout with count at the end. |
| `fib` | `fib.c` | First 30 Fibonacci numbers. Formatted as `F(N) = value`, one per line. |
| `chars` | `chars.c` | ASCII character set dump (0x20–0x7E). Displays a hex-grid (6 rows × 15 columns) with row/column headers. Clears screen before rendering. |
| `calc` | `calc.c` | 4-function calculator (+, -, *, /). Interactive prompts: `num1>`, `op>`, `num2>`. Supports negative numbers. Type `q` to quit. |
| `guess` | `guess.c` | Number guessing game (1–100). Seeds a simple LCG PRNG, gives "too low"/"too high" hints, counts attempts. |

All programs run via the MINI executable loader (`exec.c`). The build script (`build_64_boot.sh`) compiles each program separately, links them at unique addresses under `0x100000`, and appends a 32-byte header (magic, name, code size, entry address) before concatenating into `programs.bin`.

## Bugs Fixed

### Bug 1 — Screen Overflow Causing Reboot

**Symptom:** When a program's output exceeded one screen (required scrolling), the system appeared to reboot. Programs with short output scrolled correctly; `fib` (30 lines) reliably triggered the issue.

**Root Cause:** Missing `-mno-red-zone` compiler flag.

Without `-mno-red-zone`, GCC places local variables of leaf functions (functions that don't call other functions) in the 128-byte "red zone" *below* RSP — unallocated stack space. `scroll_screen` is a leaf function: it copies 24 rows × 160 bytes = 3840 bytes inside the VGA buffer without calling any other function. All its locals (`dst`, `src`, `row_size`, loop counters) sat in the red zone.

When a keyboard interrupt fired during the copy loop, the interrupt handler's stack pushes (~176 bytes: 40 CPU-pushed + 16 stub-pushed + 120 registers) landed directly on top of `scroll_screen`'s locals. After the interrupt returned, variables like `dst` and `src` held garbage — causing writes to arbitrary memory addresses, potentially corrupting page tables, IDT, or kernel code.

**Fix:** Added `-mno-red-zone` to `CFLAGS` in `build_64_boot.sh` (line 21). `scroll_screen` now emits `sub $0x38, %rsp` to properly allocate stack space below its frame, isolating it from interrupt handler stack usage.

### Bug 2 — ISR Stubs Mishandling CPU Error Codes

**Symptom:** Any CPU exception that pushes an error code (#DF, #TS, #NP, #SS, #GP, #PF, #AC) would cause a triple fault (reboot) because `iretq` read the CPU's error code as RIP.

**Root Cause:** For exception vectors 8, 10, 11, 12, 13, 14, 17, the CPU already pushes an error code onto the stack. The stub macro unconditionally pushed a *second* dummy `$0`. The common stub's `addq $16, %rsp` skipped the first two quadwords (`$\num` + dummy `$0`), leaving the CPU's real error code at the `iretq` RIP position — the CPU then tried to "return" to the error code value as an instruction address.

**Fix:** Created `isr_stub_err` macro in `arch/isr.S` that only pushes `$\num` (no dummy `$0`). Used for vectors 8, 10, 11, 12, 13, 14, 17. The `addq $16, %rsp` in the common stub now correctly skips `$\num` + `error_code` in all paths, converging on the proper RIP for `iretq`.

## Known Issue / TODO — calc: Key Combinations Not Supported

The `calc` program reads operators via `keyboard_read()`, but the keyboard driver (`drivers/keyboard.c`) maps only *unmodified* PS/2 scancodes. On a US keyboard:

- `-` is a dedicated key (scancode 0x0C) → works
- `/` is a dedicated key (scancode 0x35) → works
- `+` is **Shift + `=`** → requires shift tracking → **not available**
- `*` is **Shift + `8`** (standard position) → requires shift tracking → **not available**
  - (Keypad `*` at scancode 0x37 *is* mapped, but most users expect the standard key)

The keyboard driver currently has no shift-state tracking: scancodes 0x2A (LShift) and 0x36 (RShift) are ignored on press, and there is no modifier layer to remap keys based on shift state.

**TODO:** Add shift-state tracking to the keyboard ISR so that `Shift + =` produces `+` and `Shift + 8` produces `*`. This would require:
1. Tracking LShift/RShift press/release (scancodes 0x2A/0xAA, 0x36/0xB6)
2. Applying an alternate scan-to-ASCII mapping when shift is held
3. Defining shifted variants in the lookup table
