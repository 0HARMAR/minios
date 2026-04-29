# RECORD5.1 — Keyboard Shift-State Tracking

Resolves the TODO from RECORD5.md: "calc: Key Combinations Not Supported."

## Changes

### `long-mode/drivers/keyboard.c` — Shift-state tracking in the keyboard ISR

**Problem:** The `calc` program could not accept `+` or `*` input because the keyboard driver had no concept of modifier keys. `+` requires `Shift+=` and `*` requires `Shift+8` on a US keyboard, but the driver only mapped unmodified scancodes.

**Implementation:**

1. **Shift press/release tracking** — The ISR now catches:
   - `0x2A` (LShift press), `0x36` (RShift press) → sets `shift_pressed = 1`
   - `0xAA` (LShift release), `0xB6` (RShift release) → sets `shift_pressed = 0`
   - These scancodes are consumed by the handler and never enqueued into the key buffer.

2. **Shifted lookup table** (`scan_to_ascii_shifted[128]`) — A second scan-to-ASCII table defines the standard US QWERTY shifted variants for all keys:
   - Number row: `!@#$%^&*()_+`
   - Letters: uppercase `A`–`Z`
   - Symbols: `{}|:"~<>?`
   - Extended mappings (Enter, Backspace, Space) are duplicated unchanged.

3. **Table selection** — When `shift_pressed` is set, the handler reads from `scan_to_ascii_shifted` instead of `scan_to_ascii`. The selection is done per-scancode, so the shift state at the moment of each keypress determines which character is produced.

**Key mappings relevant to `calc`:**

| Chord | Scancode | Unshifted | Shifted |
|-------|----------|-----------|---------|
| `=` | 0x0D | `=` | `+` |
| `8` | 0x09 | `8` | `*` |

**Build:** Compiles cleanly with `build_64_boot.sh`, no warnings.
