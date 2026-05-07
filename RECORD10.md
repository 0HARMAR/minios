# RECORD10 — Kernel/User Memory Paging Isolation

## Goal

Split the identity-mapped 1 GB address space at the page-table level so ring 3
(user) code cannot read or write kernel memory.  The split uses the x86_64 U/S
(User/Supervisor) bit at the PD level — the simplest possible change: no new
page-table pages, no higher-half kernel, no syscall infrastructure.

## Background

Before this change, every page-table entry (PML4[0], PDP[0], all 512 PD
entries) carried `PAGE_USER`.  Ring 3 could access the entire first 1 GB of
physical memory — kernel code, page tables, GDT, IDT, ISR stubs, everything.

The U/S bit is checked at **every** level of the hardware page walk.  If any
level has U/S=0, ring 3 gets a #PF.  Ring 0 always passes.  The walk itself
uses physical addresses (not subject to the U/S check for the page tables'
own storage), so as long as PML4[0] and PDP[0] have `PAGE_USER`, the
hardware can walk through them to reach user-accessible PD entries.

## Design

Single PML4 → single PDP → single PD, control at the PD leaf:

```
PML4[0]  →  PDP[0]   (PAGE_USER kept on both — walk must pass for user addrs)
                |
                v
             PD[0]   : 0x0      – 0x1FFFFF   Supervisory (no PAGE_USER)
             PD[1]   : 0x200000 – 0x3FFFFF   User
             PD[2]   : 0x400000 – 0x5FFFFF   User
             ...
             PD[511] : 0x3FE00000–0x3FFFFFFF User
```

**Why PROGRAMS_BASE had to move.**  `0x100000` (1 MB) sits in the middle of
the first 2 MB huge page (`0x0`–`0x1FFFFF`).  2 MB huge pages cannot split
within one entry.  Moving the user boundary to `0x200000` (2 MB) aligns it
with the next huge-page boundary.

### Memory layout after the change

| Physical / Virtual | Size | PD entry | U/S | Owner |
|---|---|---|---|---|
| 0x000000–0x1FFFFF | 2 MB | pd[0] | S | kernel (code, data, bss, page tables, GDT, IDT, ISR, stack, VGA) |
| 0x200000–0x3FFFFF | 2 MB | pd[1] | U | user programs (PROGRAMS_BASE) |
| 0x400000–0x5FFFFF | 2 MB | pd[2] | U | user stack (ex-0x300000) |
| 0x600000–0x3FFFFFFF | 1018 MB | pd[3..511] | U | rest |

## Changes

### 1. `boot_64.S` — PD fill loop split

Old: all 512 PD entries got `PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE | PAGE_USER`.

New: PD[0] gets no `PAGE_USER` (supervisor-only); PD[1..511] keep `PAGE_USER`.

```asm
# PD[0] — kernel (no PAGE_USER → ring3 cannot access)
movl $0, (%edi)
orl $(PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE), (%edi)
addl $8, %edi

# PD[1..511] — user, 511 * 2MB = 1022MB
movl $0x200000, %eax
movl $511, %ecx
1:
    movl %eax, (%edi)
    orl $(PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE | PAGE_USER), (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop 1b
```

PML4[0] and PDP[0] unchanged — both keep `PAGE_USER` so the hardware walker
passes the U/S check when ring 3 accesses user addresses.

### 2. `kernel/exec.h` — move user program base

```c
#define PROGRAMS_BASE  0x200000   /* was 0x100000 */
```

### 3. `build_64_boot.sh` — move program link `BASE`

```bash
PROGRAMS_BASE=0x200000           # was 0x100000
```

All 7 user programs now link at `0x200040`, `0x200135`, etc.

### 4. `kernel/kernel.c` — move user stack

```c
enter_user_mode((void *)user_program, (void *)0x400000);  /* was 0x300000 */
```

## What ring 3 can no longer do

Any ring 3 access to virtual addresses below `0x200000` causes #PF.  This
includes:
- Direct VGA writes (`0xB8000`) — all 7 user programs and `arch/user.S:user_program` do this
- Reading kernel data structures
- Touching page tables
- Accessing kernel stack

The `user` shell command and all `exec()`-launched programs will fault on
their first VGA write.  The next step is a syscall layer so user code can
print via the kernel, or at minimum a #PF handler that reads CR2 for a
diagnostic dump.
