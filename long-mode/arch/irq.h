#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

struct registers {
    uint64_t r15, r14, r13, r12, r11, r10;
    uint64_t r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t interrupt, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct registers *);

void irq_register_handler(uint8_t irq, irq_handler_t handler);
void irq_handler(struct registers *r);
void irq_init(void);

#endif