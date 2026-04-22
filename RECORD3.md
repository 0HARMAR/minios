# RECORD3: Keyboard Interrupt Refactor

## Date: 2026-04-22

## Summary
Converted keyboard input from polling mode to interrupt-driven mode using the 8259A PIC.

## Changes

### Before
- Kernel polled port 0x64 (keyboard status) in a tight loop
- Read scancode from port 0x60 when available
- CPU constantly busy-waiting, inefficient

### After
- Keyboard generates IRQ1 on keypress
- ISR saves registers, dispatches to handler
- Handler reads scancode, stores in ring buffer
- Kernel reads from buffer (blocking or non-blocking)

## Files Added

### Architecture (`long-mode/arch/`)
| File | Description |
|------|-------------|
| `pic.c` / `pic.h` | 8259A PIC initialization and control |
| `idt.c` / `idt.h` | Interrupt Descriptor Table setup (256 entries) |
| `isr.S` | Assembly ISR stubs for all 256 vectors |
| `irq.c` / `irq.h` | IRQ handler dispatch and registration |

### Drivers (`long-mode/drivers/`)
| File | Description |
|------|-------------|
| `keyboard.c` / `keyboard.h` | Interrupt-driven keyboard with ring buffer |
| `vga.c` / `vga.h` | VGA text mode driver |

### Kernel (`long-mode/kernel/`)
| File | Description |
|------|-------------|
| `kernel.c` | Main kernel (removed polling loop, uses `keyboard_available()` / `keyboard_read()`) |
| `interrupts.c` / `interrupts.h` | Unified initialization function |

### Include (`long-mode/include/`)
| File | Description |
|------|-------------|
| `io.h` | I/O port functions |
| `keyboard.h` | Keyboard API |
| `vga.h` | VGA API |
| `idt.h` | IDT API |
| `irq.h` | IRQ API |
| `pic.h` | PIC API |
| `interrupts.h` | Interrupts API |

### I/O (`long-mode/io/`)
| File | Description |
|------|-------------|
| `io.c` | Low-level I/O port read/write |

## Build Scripts Modified

| File | Change |
|------|--------|
| `long-mode/build_64_boot.sh` | Added new source files to build from new directory structure |

## Project Structure

```
long-mode/
├── arch/           # Architecture-specific code
│   ├── idt.c/h     # Interrupt Descriptor Table
│   ├── irq.c/h     # IRQ handling
│   ├── pic.c/h     # 8259A PIC
│   └── isr.S       # ISR assembly stubs
├── drivers/        # Device drivers
│   ├── keyboard.c/h
│   └── vga.c/h
├── include/        # Public headers
│   ├── io.h
│   ├── keyboard.h
│   ├── vga.h
│   ├── idt.h
│   ├── irq.h
│   ├── pic.h
│   └── interrupts.h
├── io/             # Low-level I/O
│   └── io.c
├── kernel/         # Kernel core
│   ├── kernel.c
│   └── interrupts.c/h
├── boot_*.S        # Boot assembly
└── build_64_boot.sh
```

## Technical Details

### PIC Configuration
- Master PIC: IRQ0-7 → vectors 0x20-0x27
- Slave PIC: IRQ8-15 → vectors 0x28-0x2F
- Keyboard (IRQ1) unmasked, maps to vector 0x21

### IDT Entry
- 64-bit gate descriptor (present, DPL=0, interrupt gate)
- Selector: 0x08 (kernel code segment)

### ISR Flow
1. Interrupt fires → CPU pushes error code + vector
2. Assembly stub saves all registers
3. Calls `irq_handler(registers*)` in C
4. Handler dispatches to registered IRQ handler
5. Sends EOI to PIC
6. Restores registers, `iretq`

### Keyboard Ring Buffer
- 256-byte circular buffer
- Non-blocking check: `keyboard_available()`
- Blocking read: `keyboard_read()` (waits until char available)

## Build
```bash
cd long-mode
./build_64_boot.sh
# Output: output/disk.img
```