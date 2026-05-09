# RECORD11 — First System Call: `output(dd, buf, len)` via `int $0x80`

## Goal

Add the first system call so ring 3 code can perform I/O through the kernel
instead of faulting on direct hardware access.  Where [RECORD10](RECORD10.md)
locked ring 3 out of the supervisor-only region, this record builds the door
ring 3 is supposed to knock on.

## Background

After RECORD10 (paging isolation), PD[0] (0x0–0x1FFFFF) has no `PAGE_USER`.
Ring 3 cannot touch VGA at `0xB8000`, kernel code, page tables, or any other
kernel data.  The `user` shell command and all `exec()`-launched programs
would fault on their first I/O attempt.

The design doc is [DESIGN_SYSCALL.md](DESIGN_SYSCALL.md).

## Design

### Mechanism: `int $0x80` software interrupt gate

One IDT gate (DPL=3) + one handler function.  No MSR writes, no GDT reorder.
The same handler body will work unchanged when upgrading to `syscall`/`sysret`
later — only the entry trampoline changes.

### Syscall ABI

```
RAX  = syscall number (in) / return value (out)
RDI  = arg0 — device descriptor dd
RSI  = arg1 — buffer pointer buf
RDX  = arg2 — byte count len
RCX, R11 = clobbered (for future syscall/sysret compatibility)
```

Syscall number: `SYS_OUTPUT = 1`.

### Device Descriptor Table

| dd | Device | Function |
|----|--------|----------|
| 0  | stdout / VGA | `vga_write(buf, len)` |

### Buffer Validation

Range check: `buf[0..len)` must lie entirely within `[0x200000, 0x40000000)`
(user-accessible region).  Overflow check prevents wraparound attacks.

### Error Codes

```
EBADF  = 1   (bad device descriptor)
EFAULT = 2   (bad user memory reference)
```

Returned as negative values: `return -EBADF`, `return -EFAULT`.

## Call Flow

```
User program (ring 3) @ 0x300000
  │
  ├─ RAX=1, RDI=0, RSI=msg, RDX=len
  ├─ int $0x80
  │
  ▼  CPU: switch ring 0 via IDT[0x80] (DPL=3)
  │        push SS:RSP:RFLAGS:CS:RIP → kernel stack (TSS.RSP0=0x90000)
  │        load CS:RIP from IDT gate → irq_stub_80
  │
irq_stub_80 (arch/isr.S)
  │  push $0 (dummy error), push $0x80 (vector), jmp common_stub
  │
common_stub (arch/isr.S)
  │  push all GPRs, mov %rsp→%rdi, call irq_handler
  │
irq_handler (arch/irq.c)
  │  interrupt == 0x80 → syscall_handler(regs)   (no EOI — software int)
  │
syscall_handler (kernel/syscall.c)
  │  switch (regs->rax):
  │    case SYS_OUTPUT:
  │      sys_output(regs->rdi, regs->rsi, regs->rdx)
  │        → validate_user_buffer(buf, len)
  │        → device_table[dd](buf, len) = vga_write(...)
  │  regs->rax = bytes_written
  │
common_stub (continued)
  │  pop all GPRs, addq $16, iretq
  │
  ▼  CPU: pop RIP:CS:RFLAGS:RSP:SS → return ring 3
User program resumes, RAX = bytes written
```

## Changes

### 1. `kernel/syscall.h` — new

Syscall numbers, error codes, handler declaration.  Includes `irq.h` for the
`struct registers` definition.

### 2. `kernel/syscall.c` — new

- `vga_write(buf, len)` — writes chars to VGA, handles `\n`/`\r`, updates cursor
- `device_table[]` — static array of `dev_write_t` function pointers (dd=0 → vga_write)
- `validate_user_buffer(buf, len)` — range + overflow check against `[0x200000, 0x40000000)`
- `sys_output(dd, buf, len)` — validates dd, validates buffer, dispatches to device
- `syscall_handler(regs)` — switch on `regs->rax`, writes return value back

### 3. `arch/idt.c` — one gate re-set

After the standard loop fills all 256 gates with DPL=0, vector 0x80 is
overwritten with `type_attr=0xEE` (present, DPL=3, interrupt gate).  Ring 3
can now trigger `int $0x80` without a #GP.

### 4. `arch/irq.c` — dispatch before IRQ logic

`irq_handler` checks `r->interrupt == 0x80` first.  If so, calls
`syscall_handler(r)` and returns without sending EOI (software interrupts
don't need PIC acknowledgment).

### 5. `arch/user.S` — ring 3 syscall stub

Added position-independent `syscall_user_program` after the old
`user_program` (kept as dead code):

```asm
syscall_user_program:
    movq    $1, %rax            # SYS_OUTPUT
    movq    $0, %rdi            # dd = stdout
    leaq    msg(%rip), %rsi     # buf (RIP-relative → position-independent)
    movq    $msg_len, %rdx      # len
    int     $0x80
2:  jmp     2b                  # idle loop (NOT hlt — privileged in ring 3)

msg:
    .ascii "hello, world from ring 3!\n"
    .set msg_len, . - msg
```

Two exported symbols: `syscall_user_program` and `syscall_user_program_end`
mark the start and end so the kernel knows how many bytes to copy.

### 6. `kernel/kernel.c` — copy stub to user memory

The stub is linked inside the kernel binary (< 0x200000, supervisor-only).
At runtime, the "user" command copies it to `0x300000` (inside user-accessible
PD[1]) before calling `enter_user_mode`:

```c
uint64_t stub_sz = (uint64_t)(syscall_user_program_end - syscall_user_program);
for (uint64_t i = 0; i < stub_sz; i++)
    ((char *)0x300000)[i] = syscall_user_program[i];
enter_user_mode((void *)0x300000, (void *)0x400000);
```

The RIP-relative addressing in the stub ensures the `leaq msg(%rip)`
instruction resolves correctly wherever the code executes.

### 7. `kernel/userlib.h` — new

Thin inline wrapper for future ring-3 C programs:

```c
static inline int64_t output(uint8_t dd, const void *buf, uint64_t len) {
    int64_t ret;
    asm volatile(
        "movq $1, %%rax\n"
        "int  $0x80\n"
        : "=a"(ret)
        : "D"((uint64_t)dd), "S"((uint64_t)buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}
```

### 8. `build_64_boot.sh` — build syscall.c

Added compilation of `kernel/syscall.c` and inclusion of `syscall.o` in the
kernel link step.

## Design decisions

| Decision | Why |
|----------|-----|
| `int $0x80` over `syscall` | Minimum change: one IDT gate, no MSR or GDT changes. Handler body is identical for both paths. |
| Syscall number in RAX | Standard Linux/x86-64 ABI; trivially migratable to `syscall` instruction. |
| Position-independent user stub | The stub lives in kernel text (< 0x200000) at link time but runs from user memory (0x300000) at runtime. RIP-relative addressing makes this work. |
| Buffer bounds check (range-based) | Sufficient for identity-mapped memory. Extends to page-walk validation later. |
| Static device descriptor table | Only 1 device exists (VGA). A dynamic fd table is overengineering. |
| Separate `SYS_OUTPUT` not `SYS_WRITE` | Device descriptors and file descriptors are different namespaces for now. |
| Paging isolation preserved | PD[0] stays supervisor-only. The user stub runs from PD[1] (0x300000). |

## What this enables

With the syscall infrastructure in place, subsequent syscalls (`input`,
`open`, `read`, `write`, `exit`) reuse the entry/exit path, dispatch table,
and buffer validation.  Only the handler body changes.

## Build verification

```
$ bash build_64_boot.sh
Kernel:   31488 bytes
Programs: 6087 bytes
Payload:  37575 bytes
Build complete.
Run: qemu-system-x86_64 -hda output/disk.img
```

## File summary

| File | Action |
|------|--------|
| `long-mode/kernel/syscall.h` | New — syscall numbers, error codes, declaration |
| `long-mode/kernel/syscall.c` | New — handler, dispatch, device table, buffer validation |
| `long-mode/kernel/userlib.h` | New — inline wrapper for ring-3 C programs |
| `long-mode/arch/idt.c` | Modified — vector 0x80 gate set to DPL=3 (0xEE) |
| `long-mode/arch/irq.c` | Modified — special-case interrupt 0x80, skip EOI |
| `long-mode/arch/user.S` | Modified — added syscall_user_program and user_program_end |
| `long-mode/kernel/kernel.c` | Modified — copy stub to 0x300000, enter ring 3 |
| `long-mode/build_64_boot.sh` | Modified — compile and link syscall.c |
| `long-mode/boot_64.S` | Unchanged — paging isolation preserved |
