# RECORD8 — Ring 0 to Ring 3 User Mode

## Goal

Switch the CPU from ring 0 (kernel) to ring 3 (user) via `iretq` and execute a
minimal user program that writes `"hello, world"` directly to VGA memory, then
loops forever.  This establishes the privilege-separation foundation for future
syscall/scheduler work.

## Design

Ring switching on x86-64 requires four pieces wired together:

1. **GDT** — ring 3 code/data descriptors (DPL=3) so the CPU has segments to
   load into CS:SS when `iretq` pops the target frame.
2. **TSS** — a 64-bit Task State Segment holding `RSP0` (the ring 0 stack
   pointer the CPU uses when an interrupt fires in ring 3).  Loaded via `ltr`.
3. **Paging** — the U/S bit set in **every** page-table level (PML4, PDP, PD)
   for pages that user code must access.  One US=0 at any level blocks ring 3.
4. **`iretq` frame** — push SS, RSP, RFLAGS, CS, RIP onto the kernel stack;
   `iretq` pops all five and enters ring 3.

### GDT layout

| Entry | Selector | Purpose |
|-------|----------|---------|
| 0 | `0x00` | Null |
| 1 | `0x08` | Ring 0 code (64-bit) |
| 2 | `0x10` | Ring 0 data |
| 3 | `0x18` → `0x1B` (RPL=3) | Ring 3 code (64-bit, execute/read) |
| 4 | `0x20` → `0x23` (RPL=3) | Ring 3 data (read/write) |
| 5-6 | `0x28` | TSS descriptor (16 bytes, filled at runtime) |

### `iretq` frame (bottom to top)

```
RIP  = &user_program     ←  popped first
CS   = 0x1B              ←  ring 3 code, RPL=3
RFLAGS  = current|IF=1   ←  preserved reserved bits via pushfq
RSP  = 0x300000          ←  user stack top
SS   = 0x23              ←  ring 3 data, RPL=3    ←  popped last
```

### Memory layout

| Address | Purpose |
|---------|---------|
| `0x979a` | `user_program` code (kernel .text, identity-mapped) |
| `0xB8000` | VGA text buffer (written directly by user code) |
| `0x300000` | User stack top (unused by this program) |
| `0x90000` | Kernel stack top → stored in TSS.RSP0 |
| `0xF848` | TSS structure (104 bytes, in .data) |

## Changes

### 1. `boot_64.S` — page tables + GDT

- Added `.set PAGE_USER, (1 << 2)`
- Set `PAGE_USER` on **PML4[0]**, **PDP[0]**, and all **512 PD entries** so the
  entire 1 GB identity-mapped region is accessible from ring 3
- Extended GDT from 3 entries to 7: added ring 3 code (`0x0020FA0000000000`),
  ring 3 data (`0x0000F20000000000`), and two TSS placeholders (filled by C)
- Exported `gdt64` symbol for C

### 2. New file `arch/user.S` — ring 3 program + entry trampoline

- `user_program` — writes `"hello, world"` character-by-character to VGA at
  `0xB8000`, then `jmp .` (infinite loop)
- `enter_user_mode` — loads TR via `ltr $0x28`, builds the 5-qword stack frame
  with `pushfq` for valid RFLAGS, executes `iretq`
- `tss` — 104-byte zeroed TSS in `.data`

### 3. `kernel/kernel.c` — TSS init + shell command

- `tss_init()` — zeros TSS, sets `RSP0 = 0x90000`, sets IOPB offset = 104
  (past TSS limit = no I/O bitmap), fills TSS descriptor in GDT[5] and GDT[6]
- Called during `kernel_main` init sequence
- Added `"user"` command: `enter_user_mode(user_program, (void *)0x300000)`

### 4. `build_64_boot.sh` — build integration

- Assembles `arch/user.S` → `user.o`, links into kernel

## Bugs found and fixed

### Bug 1 — PML4/PDP missing U/S bit (blocked ring 3 access)

**Symptom:** typing `user` caused a page fault (#PF, vector 14, CR2=`0x979a`)
with error code `0x5` (page-protection violation on a present user-access page).

**Cause:** `PAGE_USER` was set on all PD entries (`orl $0x87`), but PML4[0]
and PDP[0] were set up with only `PAGE_PRESENT | PAGE_WRITE` (`0x23`).  In
x86-64 the CPU checks U/S at **every** page-walk level; one US=0 at PML4 or
PDP blocks the entire 512 GB / 1 GB subtree from ring 3.

**Fix** (`boot_64.S` lines 32, 37): changed both to
`orl $(PAGE_PRESENT | PAGE_WRITE | PAGE_USER)`.

### Bug 2 — IOPB offset at wrong position

**Cause:** `tss_init()` wrote the I/O Permission Bitmap offset to `tss + 98`
instead of `tss + 100` (the correct field in the 64-bit TSS layout).

**Fix** (`kernel.c`): corrected to `*(uint16_t *)(tss + 100) = 104`.

**Impact of bug:** the actual IOPB field at offset 100 remained 0, meaning the
CPU treated the entire TSS as the I/O permission bitmap.  This did not affect the
demo (no I/O instructions are used), but was corrected for correctness.

## Debugging process

Used GDB via QEMU (`-s -S`) with the ELF symbol file `output/stage2.elf`.
Wasolating the failure involved four diagnostic builds:

| Test | ltr | CS/SS target | RFLAGS | Result |
|------|-----|-------------|--------|--------|
| Original | yes | ring 3 | hardcoded `0x202` | frozen (exception loop) |
| No-ltr | no | ring 3 | `pushfq` | reboot (timer triple-fault → proved `iretq` worked) |
| Ring 0 test | yes | **ring 0** | `pushfq` | hello printed (proved frame correct, privilege-change was failing) |
| GDB step | yes | ring 3 | `pushfq` | hit `isr_stub_14` (#PF) → CR2=`0x979a`, error `0x5` → traced to PML4/PDP |

## Architecture

```
  Shell types "user"
        │
        v
  enter_user_mode()
    ├─ ltr 0x28          ← loads TSS (CPU now knows ring0 stack for interrupts)
    ├─ pushq $0x23       ← SS
    ├─ pushq %rsi        ← RSP  (0x300000)
    ├─ pushfq            ← RFLAGS
    ├─ orq $0x200,(%rsp) ← set IF
    ├─ pushq $0x1B       ← CS
    ├─ pushq %rdi        ← RIP  (&user_program)
    └─ iretq
         │
         v
    ┌──────────────────────┐
    │  ring 3 execution     │
    │  user_program()       │
    │    write to 0xB8000   │──▶ "hello, world" on screen
    │    jmp .              │
    └──────────────────────┘
         │  timer IRQ fires
         v
    CPU reads TSS.RSP0 → switches to ring0 stack (0x90000)
    isr_stub_32 → irq_handler → timer_handler → pic_send_eoi
    iretq → back to ring 3
```

## File summary

| File | Action |
|------|--------|
| `boot_64.S` | Added PAGE_USER define; set U/S on PML4, PDP, and PD; extended GDT to 7 entries; exported gdt64 |
| `arch/user.S` | **New** — user program, enter_user_mode trampoline, TSS structure |
| `kernel/kernel.c` | tss_init() + "user" shell command |
| `build_64_boot.sh` | Assemble + link user.o |
