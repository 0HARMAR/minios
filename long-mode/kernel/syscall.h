#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "irq.h"

/* syscall numbers */
#define SYS_OUTPUT  1
#define SYS_INPUT   2

/* device descriptors */
#define DD_STDOUT   0
#define DD_KEYBOARD 1

/* error codes (returned as negative values) */
#define EBADF   1
#define EFAULT  2

/* dispatched from irq_handler for software interrupt 0x80 */
void syscall_handler(struct registers *regs);

#endif
