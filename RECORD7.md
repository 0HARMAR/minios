# RECORD7 — 8253/8254 PIT Timer for Long Mode

## Goal

Add programmable interval timer (PIT) support to the long-mode kernel so we can track
uptime and sleep for measured intervals. In QEMU this is the classic 8253/8254 PIT wired
to IRQ0 on the 8259A PIC.

## Changes

### 1. Bug fix — `arch/pic.c`

- **`pic_send_eoi()`**: was writing to `PIC1_COMMAND` twice for IRQ ≥ 8 instead of
  sending EOI to `PIC2_COMMAND` first. Fixed.
- **PIC1 initial mask**: changed from `0xFD` (only IRQ1/keyboard enabled) to `0xFC`
  (both IRQ0/timer and IRQ1/keyboard enabled).

### 2. New driver — `drivers/timer.h` + `drivers/timer.c`

PIT channel 0 configured at **100 Hz** in rate-generator mode (mode 2):

| Parameter | Value |
|-----------|-------|
| Base frequency | 1,193,182 Hz |
| Divisor | 11,932 |
| Resulting rate | 100 ticks/second |
| Command byte | `0x34` (CH0, lobyte/hibyte, mode 2, binary) |
| Data port | `0x40` |
| Command port | `0x43` |

IRQ0 handler increments a 64-bit volatile tick counter.

Public API:
- `timer_init()` — register IRQ0 handler, enable IRQ0, program PIT
- `timer_get_ticks()` → `uint64_t` — ticks since boot
- `timer_wait(n)` — busy-wait for N ticks

### 3. Shell commands — `kernel/kernel.c`

- **`uptime`** — prints the current tick count
- **`sleep N`** — busy-waits for N ticks (e.g., `sleep 100` ≈ 1 second)
- Added "uptime sleep" to the `help` output

### 4. Build — `build_64_boot.sh`

Added `timer.o` to the compile list and linker object list.

## Architecture

```
                    ┌──────────┐
1.193182 MHz ──────▶│   PIT    │── IRQ0 ──▶ PIC ── INT 0x20 ──▶ CPU (isr_stub_32)
                    │ Channel 0│                                     │
                    └──────────┘                              irq_handler()
                      divisor=11932                                 │
                      fires @ 100 Hz                         timer_handler()
                                                               ticks++
```

## File summary

| File | Action |
|------|--------|
| `arch/pic.c` | Fixed EOI bug + IRQ0 mask |
| `drivers/timer.h` | New — timer API |
| `drivers/timer.c` | New — PIT init + IRQ0 handler |
| `kernel/kernel.c` | `timer_init()` call + `uptime`/`sleep` commands |
| `build_64_boot.sh` | Compile + link `timer.o` |
