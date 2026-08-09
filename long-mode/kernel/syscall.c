#include "vga.h"
#include "keyboard.h"
#include "syscall.h"
#include "irq.h"
#include "process.h"
#include "paging.h"
#include "lib.h"
#include "io.h"
#include "e1000.h"

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
        case '\b':
            if (vga_cursor_pos >= 2)
                vga_cursor_pos -= 2;
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

    asm volatile("sti");

    p[0] = keyboard_read();

    uint64_t n = 1;
    while (n < len && keyboard_available())
        p[n++] = keyboard_read();

    return (int64_t)n;
}

/* ── device: serial port (COM1, I/O 0x3F8) ──────────────────────── */
#define COM1_PORT 0x3F8

static int serial_inited;
static void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);   /* disable interrupts */
    outb(COM1_PORT + 3, 0x80);   /* DLAB=1 to set baud divisor */
    outb(COM1_PORT + 0, 0x03);   /* divisor low  = 3  → 38400 baud */
    outb(COM1_PORT + 1, 0x00);   /* divisor high = 0 */
    outb(COM1_PORT + 3, 0x03);   /* 8N1, DLAB=0 */
    serial_inited = 1;
}

int64_t serial_write(const void *buf, uint64_t len) {
    if (!serial_inited) serial_init();
    const char *p = (const char *)buf;
    for (uint64_t i = 0; i < len; i++) {
        if (p[i] == '\n') {
            while (!(inb(COM1_PORT + 5) & 0x20)) { }
            outb(COM1_PORT, '\r');
        }
        while (!(inb(COM1_PORT + 5) & 0x20)) { }
        outb(COM1_PORT, p[i]);
    }
    return (int64_t)len;
}

/* ── device descriptor tables ─────────────────────────────────── */
static dev_write_t write_table[MAX_DEVICES] = {
    [DD_STDOUT]   = vga_write,
    [DD_COM1]     = serial_write,
};

static dev_read_t read_table[MAX_DEVICES] = {
    [DD_KEYBOARD] = keyboard_read_buf,
};

/* ── buffer validation (per-process bounds) ───────────────────── */
static int validate_user_buffer(const void *buf, uint64_t len) {
    uint64_t start = (uint64_t)buf;
    uint64_t end   = start + len;

    if (end < start)
        return -EFAULT;

    /* use per-process bounds if running a user process */
    if (current != 0 && current->pid != 0) {
        if (start < current->user_start || end > current->user_end)
            return -EFAULT;
    } else {
        /* fallback for shell (pid 0) — kernel range only */
        if (start < 0x200000 || end > 0x40000000)
            return -EFAULT;
    }

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

static int64_t sys_crtproc(const char *path, uint64_t flags) {
    if (flags != 0)
        return -EINVAL;
    if (validate_user_buffer(path, 128) != 0)
        return -EFAULT;

    return proc_spawn(path);
}

static int64_t sys_netsend(const void *buf, uint64_t len) {
    if (len > 1518)
        return -EINVAL;
    if (validate_user_buffer(buf, len) != 0)
        return -EFAULT;
    if (e1000_send((const uint8_t *)buf, (uint16_t)len) != 0)
        return -1;
    return (int64_t)len;
}

static int64_t sys_netrecv(void *buf, uint64_t len) {
    if (validate_user_buffer(buf, len) != 0)
        return -EFAULT;
    return (int64_t)e1000_recv((uint8_t *)buf, (uint16_t)(len > 65535 ? 65535 : len));
}

static int64_t sys_netmac(void *buf, uint64_t len) {
    if (len < 6)
        return -EINVAL;
    if (validate_user_buffer(buf, 6) != 0)
        return -EFAULT;
    uint8_t *dst = (uint8_t *)buf;
    uint8_t *mac = e1000_get_mac();
    for (int i = 0; i < 6; i++)
        dst[i] = mac[i];
    return 6;
}

void proc_exit_current(int64_t status) {
    if (current == 0 || current->pid == 0)
        return;

    current->state       = PROC_ZOMBIE;
    current->exit_status = status;

    /* free resources */
    pt_destroy(current->cr3);
    kstack_free(current->pid);

    schedule();
    asm ("sti");
    for(;;);
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
    case SYS_CRTPROC:
        regs->rax = sys_crtproc(
            (const char *)regs->rdi,
            regs->rsi);
        break;
    case SYS_NETSEND:
        regs->rax = sys_netsend(
            (const void *)regs->rdi,
            regs->rsi);
        break;
    case SYS_NETRECV:
        regs->rax = sys_netrecv(
            (void *)regs->rdi,
            regs->rsi);
        break;
    case SYS_NETMAC:
        regs->rax = sys_netmac(
            (void *)regs->rdi,
            regs->rsi);
        break;
    case SYS_EXIT:
        proc_exit_current((int64_t)regs->rdi);
        break;
    default:
        regs->rax = -1;
        break;
    }
}
