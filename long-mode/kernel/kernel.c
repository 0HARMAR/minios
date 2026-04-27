#include <stdint.h>
#include "io.h"
#include "vga.h"
#include "interrupts.h"
#include "keyboard.h"
#include "exec.h"

__attribute__((section(".data"))) uint32_t vga_cursor_pos;

void kernel_main() {
    unsigned char *vga_buffer = (unsigned char*)VGA_BUFFER;

    clear_screen(vga_buffer);

    vga_cursor_pos = 0;
    print_string(vga_buffer, "MiniOS 64-bit Shell", &vga_cursor_pos);
    vga_cursor_pos = 80 * 2 * 2;

    exec_init();
    interrupts_init();
    keyboard_init();

    print_string(vga_buffer, "$ ", &vga_cursor_pos);
    update_cursor(vga_cursor_pos);

    char cmd_buffer[128] = {0};
    uint32_t cmd_index = 0;

    while (1) {
        if (keyboard_available()) {
            char c = keyboard_read();
            if (c != '\n' && c != '\b') {
                if (cmd_index < sizeof(cmd_buffer) - 1) {
                    print_char(vga_buffer, c, &vga_cursor_pos, 0x0F);
                    cmd_buffer[cmd_index++] = c;
                }
            } else if (c == '\n') {
                cmd_buffer[cmd_index] = '\0';
                print_newline(vga_buffer, &vga_cursor_pos);

                if (cmd_index > 0) {
                    if (exec(cmd_buffer) != 0) {
                        print_string(vga_buffer, "not found: ", &vga_cursor_pos);
                        print_string(vga_buffer, cmd_buffer, &vga_cursor_pos);
                        print_newline(vga_buffer, &vga_cursor_pos);
                    } else {
                        /* program ran — skip past its output lines */
                        print_newline(vga_buffer, &vga_cursor_pos);
                        print_newline(vga_buffer, &vga_cursor_pos);
                    }
                } else {
                    print_newline(vga_buffer, &vga_cursor_pos);
                }

                print_string(vga_buffer, "$ ", &vga_cursor_pos);
                cmd_index = 0;
            } else if (c == '\b' && cmd_index > 0) {
                vga_cursor_pos -= 2;
                print_char(vga_buffer, ' ', &vga_cursor_pos, 0x0B);
                vga_cursor_pos -= 2;
                cmd_buffer[--cmd_index] = '\0';
            }
            update_cursor(vga_cursor_pos);
        }
    }
}
