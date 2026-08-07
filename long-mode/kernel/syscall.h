#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "irq.h"

/* syscall numbers */
#define SYS_OUTPUT   1
#define SYS_INPUT    2
#define SYS_CRTPROC  3
#define SYS_EXIT     4

/* device descriptors */
#define DD_STDOUT    0
#define DD_KEYBOARD  1
#define DD_COM1      2

/* error codes (returned as negative values) */
#define EBADF    1
#define EFAULT   2
#define ENOENT   3
#define ENOMEM   4
#define EINVAL   5

/* serial port write (COM1) — shared for debug tracing */
int64_t serial_write(const void *buf, uint64_t len);

/* dispatched from irq_handler for software interrupt 0x80 */
void syscall_handler(struct registers *regs);

/* exit the current process (used by page fault handler too) */
void proc_exit_current(int64_t status);

#endif
