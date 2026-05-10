# RECORD12 — Second System Call: `input(dd, buf, len)` + Deadlock Bug

## Goal

Add an `input` syscall so ring 3 code can read from devices (keyboard).
Adds `SYS_INPUT = 2` alongside the existing `SYS_OUTPUT = 1` from
[RECORD11](RECORD11.md), and extends the device descriptor table to support
both read and write operations.

## Background

With RECORD11, ring 3 could print to VGA via `output(dd=0, buf, len)`.  But
there was no way to read user input — the keyboard driver (`keyboard_read()`,
`keyboard_available()`) was only accessible from ring 0 (shell, exec'd
programs).  Ring 3 code needs a syscall path into the keyboard buffer.

## Design

### Syscall ABI (unchanged)

```
RAX  = syscall number (in) / return value (out)
RDI  = arg0 — device descriptor dd
RSI  = arg1 — buffer pointer buf
RDX  = arg2 — byte count len
```

`SYS_INPUT = 2` mirrors `SYS_OUTPUT = 1`.  Same register convention, same
buffer validation.

### Device Descriptor Tables (split read/write)

Before (RECORD11): single `device_table[]` with write-only function pointers.

After: separate `write_table[]` and `read_table[]`:

| dd | Device | write | read |
|----|--------|-------|------|
| 0  | stdout / VGA | `vga_write` | — |
| 1  | keyboard | — | `keyboard_read_buf` |

### `input(dd, buf, len)` Semantics

- Blocks until at least 1 byte is available
- Reads up to `len` bytes (drains immediately-available extras without blocking)
- Returns bytes read on success, `-EBADF` on bad dd, `-EFAULT` on bad buffer

### `keyboard_read_buf(buf, len)`

```c
// block for first char (keyboard_read is a busy-wait on buffer_head != buffer_tail)
p[0] = keyboard_read();

// drain any more immediately available, non-blocking
uint64_t n = 1;
while (n < len && keyboard_available())
    p[n++] = keyboard_read();
```

## Bug: Interrupt Deadlock (`sti` fix)

### Symptom

The `output` hello message printed correctly, but the program hung forever on
`input`.  No typed characters appeared, no echo.

### Root cause

1. `int $0x80` enters via an **interrupt gate** (type_attr=0xEE).  The CPU
   **clears IF** (disables maskable interrupts) on entry.

2. `keyboard_read()` is a busy-wait: `while (!keyboard_available())`.  It
   spins until `buffer_head != buffer_tail`.

3. Keyboard scancodes arrive via **IRQ1** (vector 33).  IRQ1 needs IF=1 to
   fire.

4. With IF=0, IRQ1 is masked → no scancodes enter the buffer →
   `keyboard_available()` never becomes true → spin-loop never exits.
   **Deadlock.**

```
ring 3:  int $0x80 (SYS_INPUT)
           │
           ▼  CPU: IF=0, switch ring 0
syscall_handler → sys_input → keyboard_read_buf → keyboard_read()
           │                                          │
           │                                     while (!available())  ← spins forever
           │                                          │
           │                                     IRQ1 can't fire (IF=0)
           ▼                                        ▼
        DEADLOCK                                DEADLOCK
```

### Fix

`asm volatile("sti")` before the first blocking `keyboard_read()` call in
`keyboard_read_buf`.  This re-enables interrupts so the keyboard IRQ can
deliver scancodes.  The busy-wait then resolves:

```
ring 3:  int $0x80 (SYS_INPUT)
           │
           ▼  CPU: IF=0, switch ring 0
syscall_handler → sys_input → keyboard_read_buf → sti (IF=1)
           │                                          │
           │                                     while (!available())
           │                                       │  ← waiting, IF=1
           │                                       │
           │  [user presses key]                    │
           │  IRQ1 fires (IF=1) → keyboard ISR     │
           │    reads scancode from port 0x60      │
           │    buffers char                        │
           │    sends EOI → iretq                   │
           │                                       │
           │                                     available()==true → break
           │                                     returns char
           │
           ▼  common_stub: iretq → ring 3
 ring 3:  RAX = 1 (char read)
```

### Why `sti` is safe here

Nested interrupts on x86-64 stack properly:
- The outer handler entered via `int $0x80` already saved state onto the
  kernel stack (TSS.RSP0=0x90000).
- A nested IRQ fires at same privilege (ring 0), so the CPU pushes
  SS:RSP:RFLAGS:CS:RIP onto the **current** RSP — not reloading TSS.RSP0.
- The nesting is shallow (one keyboard IRQ, maybe a timer tick), so the
  stack doesn't overflow.
- After the nested handler's `iretq`, we resume in `keyboard_read_buf`
  exactly where we left off, with the character now in the buffer.

## Changes

### 1. `kernel/syscall.h` — new syscall number and device constants

```c
#define SYS_OUTPUT  1
#define SYS_INPUT   2

#define DD_STDOUT   0
#define DD_KEYBOARD 1
```

### 2. `kernel/syscall.c` — read path + sti fix

- Added `dev_read_t` type for read function pointers
- Split device table into `write_table[]` and `read_table[]`
- Added `keyboard_read_buf(buf, len)` — blocks on first char via
  `keyboard_read()`, drains rest non-blocking; includes `sti` before the
  blocking call to avoid the interrupt deadlock
- Added `sys_input(dd, buf, len)` — validates dd, validates buffer,
  dispatches to `read_table`
- `syscall_handler` branches on both `SYS_OUTPUT` and `SYS_INPUT`

### 3. `kernel/userlib.h` — input wrapper

```c
static inline int64_t input(uint8_t dd, void *buf, uint64_t len) {
    int64_t ret;
    asm volatile(
        "movq $2, %%rax\n"
        "int  $0x80\n"
        : "=a"(ret)
        : "D"((uint64_t)dd), "S"((uint64_t)buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}
```

### 4. `arch/user.S` — demo: echo loop

Rewrote `syscall_user_program` to exercise both syscalls in a loop:

```
1. output(dd=0, "hello, world from ring 3!\n")
2. output(dd=0, "type a key: ")
3. input(dd=1, buf, 1)            ← blocks for keyboard input
4. output(dd=0, "you typed: ")
5. output(dd=0, buf, 1)           ← echoes the char
6. output(dd=0, "\n")
7. jmp back to line 2
```

The buffer `buf` is a single `.byte 0` in the same section, copied to user
memory along with the stub.

## File summary

| File | Action |
|------|--------|
| `long-mode/kernel/syscall.h` | Modified — added `SYS_INPUT`, `DD_STDOUT`, `DD_KEYBOARD` |
| `long-mode/kernel/syscall.c` | Modified — split read/write tables, added `keyboard_read_buf`, `sys_input`, `sti` fix |
| `long-mode/kernel/userlib.h` | Modified — added `input()` inline wrapper |
| `long-mode/arch/user.S` | Modified — rewrote stub as echo-loop demo |
| `long-mode/build_64_boot.sh` | Unchanged |

## Build verification

```
$ bash long-mode/build_64_boot.sh
Kernel:   31552 bytes
Programs: 6087 bytes
Payload:  37639 bytes
Build complete.
Run: qemu-system-x86_64 -hda long-mode/output/disk.img
```
