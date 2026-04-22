#include <stdint.h>
#include "io.h"
#include "vga.h"
#include "interrupts.h"
#include "keyboard.h"

void kernel_main() {
    unsigned char *vga_buffer = (unsigned char*)VGA_BUFFER;

    clear_screen(vga_buffer);

    uint32_t cursor_pos = 0;
    print_string(vga_buffer, "MiniOS 64-bit Shell", &cursor_pos);
    cursor_pos = 80 * 2 * 2;
    print_string(vga_buffer, "$ ", &cursor_pos);
    update_cursor(cursor_pos);

    char cmd_buffer[128] = {0};
    uint32_t cmd_index = 0;

    interrupts_init();
    keyboard_init();

    while (1) {
        if (keyboard_available()) {
            char c = keyboard_read();
            if (c != '\n' && c != '\b') {
                if (cmd_index < sizeof(cmd_buffer) - 1) {
                    print_char(vga_buffer, c, &cursor_pos, 0x0F);
                    cmd_buffer[cmd_index++] = c;
                }
            } else if (c == '\n') {
                cmd_buffer[cmd_index] = '\0';
                print_newline(vga_buffer, &cursor_pos);
                print_string(vga_buffer, "Echo: ", &cursor_pos);
                print_string(vga_buffer, cmd_buffer, &cursor_pos);
                print_newline(vga_buffer, &cursor_pos);
                print_newline(vga_buffer, &cursor_pos);
                print_string(vga_buffer, "$ ", &cursor_pos);
                cmd_index = 0;
                cmd_buffer[0] = '\0';
            } else if (c == '\b' && cmd_index > 0) {
                cursor_pos -= 2;
                print_char(vga_buffer, ' ', &cursor_pos, 0x0B);
                cursor_pos -= 2;
                cmd_buffer[--cmd_index] = '\0';
            }
            update_cursor(cursor_pos);
        }
    }
}