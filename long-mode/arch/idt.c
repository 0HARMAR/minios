#include "idt.h"

extern uint64_t isr_stub_table[];

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_desc idt_descriptor;

void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t ist, uint8_t type_attr) {
    idt[num].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[num].selector    = selector;
    idt[num].ist         = ist;
    idt[num].type_attr   = type_attr;
    idt[num].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[num].reserved    = 0;
}

void idt_init(void) {
    idt_descriptor.limit = (uint16_t)(sizeof(struct idt_entry) * IDT_ENTRIES - 1);
    idt_descriptor.base  = (uint64_t)idt;

    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, isr_stub_table[i], 0x08, 0, 0x8E);
    }
    for (int i = 32; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, isr_stub_table[i], 0x08, 0, 0x8E);
    }

    asm volatile("lidt %0" : : "m"(idt_descriptor));
}