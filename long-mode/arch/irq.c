#include "irq.h"
#include "pic.h"
#include "io.h"

extern void irq_stub_32(void);
extern void irq_stub_33(void);

static irq_handler_t irq_handlers[16] = {0};

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

void irq_handler(struct registers *r) {
    uint8_t irq = r->interrupt - 32;

    if (irq_handlers[irq] != 0) {
        irq_handlers[irq](r);
    }

    pic_send_eoi(irq);
}

void irq_init(void) {
    for (int i = 0; i < 16; i++) {
        irq_handlers[i] = 0;
    }
}