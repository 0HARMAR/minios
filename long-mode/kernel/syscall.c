#include "vga.h"
#include "syscall.h"
#include "irq.h"

#define USER_MEM_START  0x200000
#define USER_MEM_END    0x40000000   /* 1 GB */

#define MAX_DEVICES     8

/* device write function type */
typedef int64_t (*dev_write_t)(const void *buf, uint64_t len);

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

/* ── device descriptor table ──────────────────────────────────── */
static dev_write_t device_table[MAX_DEVICES] = {
    vga_write,  /* dd=0: stdout */
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

/* ── individual syscall: output(dd, buf, len) ─────────────────── */
static int64_t sys_output(uint8_t dd, const void *buf, uint64_t len) {
    if (dd >= MAX_DEVICES || device_table[dd] == 0)
        return -EBADF;

    if (validate_user_buffer(buf, len) != 0)
        return -EFAULT;

    return device_table[dd](buf, len);
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
    default:
        regs->rax = -1;  /* unknown syscall */
        break;
    }
}
