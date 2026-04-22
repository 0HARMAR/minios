#include "idt.h"
#include "pic.h"
#include "irq.h"

void interrupts_init(void) {
    idt_init();
    irq_init();
    pic_init();

    asm volatile("sti");
}