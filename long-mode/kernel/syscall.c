#include "vga.h"
#include "keyboard.h"
#include "syscall.h"
#include "irq.h"

#define USER_MEM_START  0x200000
#define USER_MEM_END    0x40000000   /* 1 GB */

#define MAX_DEVICES     8

typedef int64_t (*dev_write_t)(const void *buf, uint64_t len);
typedef int64_t (*dev_read_t)(void *buf, uint64_t len);

extern uint32_t vga_cursor_pos;

/* ── device: stdout (VGA console) ─────────────────────────────── */
static int64_t vga_write(const void *buf, uint64_t len) {
    unsigned char *vga = (unsigned char *)VGA_BUFFER;
    const char *p = (const char *)buf;

    for (uint64_t i = 0; i < len; i++) {
        switch (p[i]) {
        case '\n':
            print_newline(vga, &vga_cursor_pos);
            break;
        case '\r':
            break;
        default:
            print_char(vga, p[i], &vga_cursor_pos, 0x0F);
            break;
        }
    }
    update_cursor(vga_cursor_pos);
    return (int64_t)len;
}

/* ── device: stdin (keyboard) ─────────────────────────────────── */
static int64_t keyboard_read_buf(void *buf, uint64_t len) {
    char *p = (char *)buf;

    if (len == 0)
        return 0;

    /* keyboard_read() busy-waits; interrupts must be on for IRQ1
     * to deliver scancodes into the keyboard buffer. */
    asm volatile("sti");

    /* block for first character */
    p[0] = keyboard_read();

    /* grab any more immediately available, up to len */
    uint64_t n = 1;
    while (n < len && keyboard_available())
        p[n++] = keyboard_read();

    return (int64_t)n;
}

/* ── device descriptor tables ─────────────────────────────────── */
static dev_write_t write_table[MAX_DEVICES] = {
    [DD_STDOUT]   = vga_write,
};

static dev_read_t read_table[MAX_DEVICES] = {
    [DD_KEYBOARD] = keyboard_read_buf,
};

static int validate_user_buffer(const void *buf, uint64_t len) {
    uint64_t start = (uint64_t)buf;
    uint64_t end   = start + len;

    if (end < start)
        return -EFAULT;
    if (start < USER_MEM_START || end > USER_MEM_END)
        return -EFAULT;

    return 0;
}

/* ── syscalls ─────────────────────────────────────────────────── */
static int64_t sys_output(uint8_t dd, const void *buf, uint64_t len) {
    if (dd >= MAX_DEVICES || write_table[dd] == 0)
        return -EBADF;
    if (validate_user_buffer(buf, len) != 0)
        return -EFAULT;
    return write_table[dd](buf, len);
}

static int64_t sys_input(uint8_t dd, void *buf, uint64_t len) {
    if (dd >= MAX_DEVICES || read_table[dd] == 0)
        return -EBADF;
    if (validate_user_buffer(buf, len) != 0)
        return -EFAULT;
    return read_table[dd](buf, len);
}

/* ── syscall dispatcher ───────────────────────────────────────── */
void syscall_handler(struct registers *regs) {
    switch (regs->rax) {
    case SYS_OUTPUT:
        regs->rax = sys_output(
            (uint8_t)regs->rdi,
            (const void *)regs->rsi,
            regs->rdx);
        break;
    case SYS_INPUT:
        regs->rax = sys_input(
            (uint8_t)regs->rdi,
            (void *)regs->rsi,
            regs->rdx);
        break;
    default:
        regs->rax = -1;
        break;
    }
}
