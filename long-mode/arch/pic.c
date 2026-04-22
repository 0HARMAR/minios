#include "pic.h"
#include "io.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01

void pic_init(void) {
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);

    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);

    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, 0xFD);
    outb(PIC2_DATA, 0xFF);
}

void pic_enable_irq(uint8_t irq) {
    if (irq < 8) {
        uint8_t mask = inb(PIC1_DATA) & ~(1 << irq);
        outb(PIC1_DATA, mask);
    } else {
        uint8_t mask = inb(PIC2_DATA) & ~(1 << (irq - 8));
        outb(PIC2_DATA, mask);
    }
}

void pic_disable_irq(uint8_t irq) {
    if (irq < 8) {
        uint8_t mask = inb(PIC1_DATA) | (1 << irq);
        outb(PIC1_DATA, mask);
    } else {
        uint8_t mask = inb(PIC2_DATA) | (1 << (irq - 8));
        outb(PIC2_DATA, mask);
    }
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC1_COMMAND, 0x20);
    }
    outb(PIC1_COMMAND, 0x20);
}