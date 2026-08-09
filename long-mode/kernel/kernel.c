#include <stdint.h>
#include <stddef.h>
#include "io.h"
#include "vga.h"
#include "interrupts.h"
#include "keyboard.h"
#include "exec.h"
#include "fs.h"
#include "lib.h"
#include "timer.h"
#include "syscall.h"
#include "process.h"
#include "e1000.h"

/* user-mode entry (arch/user.S) */
extern uint64_t gdt64[];
extern uint8_t  tss[104];
extern void     enter_user_mode(void *entry, void *stack);

__attribute__((section(".data"))) uint32_t vga_cursor_pos;

static void tss_init(void) {
    /* zero TSS */
    for (int i = 0; i < 104; i++)
        tss[i] = 0;

    /* RSP0 = kernel stack top (offset 4) */
    *(uint64_t *)(tss + 4) = 0x90000;

    /* IOPB offset past TSS = no I/O bitmap (offset 100) */
    *(uint16_t *)(tss + 100) = 104;

    /* fill TSS descriptor in GDT[5] and GDT[6] */
    uint64_t addr  = (uint64_t)tss;
    uint64_t limit = 103;  /* sizeof(TSS) - 1 */

    gdt64[5] = (limit & 0xFFFFULL)
             | ((addr & 0xFFFFULL) << 16)
             | (((addr >> 16) & 0xFFULL) << 32)
             | (0x89ULL << 40)
             | (((limit >> 16) & 0xFULL) << 48)
             | (((addr >> 24) & 0xFFULL) << 56);

    gdt64[6] = (addr >> 32) & 0xFFFFFFFFULL;
}

void kernel_main() {
    unsigned char *vga_buffer = (unsigned char*)VGA_BUFFER;

    clear_screen(vga_buffer);

    vga_cursor_pos = 0;
    print_string(vga_buffer, "MiniOS 64-bit Shell", &vga_cursor_pos);
    vga_cursor_pos = 80 * 2 * 2;

    exec_init();
    interrupts_init();
    keyboard_init();
    timer_init();
    e1000_init();
    fs_init();
    tss_init();
    process_init();

    if (fs_is_ready())
        print_string(vga_buffer, "[fs ready]", &vga_cursor_pos);
    else
        print_string(vga_buffer, "[fs not ready - type mkfs]", &vga_cursor_pos);

    /* spawn ush as the primary user shell (init/systemd equivalent) */
    int64_t pid = proc_spawn("ush");
    if (pid < 0) {
        print_string(vga_buffer, "spawn ush failed", &vga_cursor_pos);
        print_newline(vga_buffer, &vga_cursor_pos);
    } else {
        print_string(vga_buffer, "ush pid ", &vga_cursor_pos);
        char num[8];
        utoa((uint32_t)pid, num);
        print_string(vga_buffer, num, &vga_cursor_pos);
        print_newline(vga_buffer, &vga_cursor_pos);
    }

    update_cursor(vga_cursor_pos);

    /* idle loop — yield to user processes, halt when nothing to do */
    while (1) {
        if (proc_has_runnable()) {
            schedule();
        } else {
            asm("sti; hlt");
        }
    }
}
