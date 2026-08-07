#include "syscall.h"
#include "irq.h"
#include "pic.h"
#include "io.h"
#include "process.h"
#include "vga.h"
#include "lib.h"

extern uint32_t vga_cursor_pos;

static irq_handler_t irq_handlers[16] = {0};

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

/* page fault (#PF, vector 14) — print diagnostic, kill user process */
static void page_fault_handler(struct registers *r) {
    uint64_t fault_addr;
    asm volatile("movq %%cr2, %0" : "=r"(fault_addr));

    unsigned char *vga = (unsigned char *)VGA_BUFFER;
    uint32_t pos = vga_cursor_pos;

    if (current == 0 || current->pid == 0) {
        print_string(vga, "KERNEL PF err=", &pos);
        char buf[32];
        utoa(r->error_code, buf);
        print_string(vga, buf, &pos);
        print_string(vga, " cs=", &pos);
        utoa(r->cs, buf);
        print_string(vga, buf, &pos);
        print_string(vga, " rip=", &pos);
        utoa((uint32_t)(r->rip >> 32), buf);
        print_string(vga, buf, &pos);
        utoa((uint32_t)r->rip, buf);
        print_string(vga, buf, &pos);
        print_string(vga, " @", &pos);
        utoa((uint32_t)(fault_addr >> 32), buf);
        print_string(vga, buf, &pos);
        utoa((uint32_t)fault_addr, buf);
        print_string(vga, buf, &pos);
        asm volatile("cli; 1: hlt; jmp 1b");
    }

    print_string(vga, "pid ", &pos);
    char buf[16];
    utoa((uint32_t)current->pid, buf);
    print_string(vga, buf, &pos);
    print_string(vga, " PF @ ", &pos);
    utoa((uint32_t)(fault_addr >> 32), buf);
    print_string(vga, buf, &pos);
    utoa((uint32_t)fault_addr, buf);
    print_string(vga, buf, &pos);

    vga_cursor_pos = pos;
    update_cursor(pos);

    proc_exit_current(-EFAULT);
}

void irq_handler(struct registers *r) {
    /* page fault — kill the process */
    if (r->interrupt == 14) {
        page_fault_handler(r);
        return;
    }

    /* software interrupt 0x80 — syscall, not an IRQ */
    if (r->interrupt == 0x80) {
        syscall_handler(r);
        if (need_resched) {
            need_resched = 0;
            schedule();
        }
        return;
    }

    uint8_t irq = r->interrupt - 32;

    if (irq_handlers[irq] != 0) {
        irq_handlers[irq](r);
    }

    pic_send_eoi(irq); 

    if (need_resched && current != 0) {
        need_resched = 0;
        schedule();
    }
}

void irq_init(void) {
    for (int i = 0; i < 16; i++) {
        irq_handlers[i] = 0;
    }
}
