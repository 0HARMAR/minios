#include <stdint.h>

#define VGA_BUFFER 0xB8000

void print_string(unsigned char *buf, const char *str, uint32_t *cursor);
void print_newline(unsigned char *buf, uint32_t *cursor);
void print_char(unsigned char *buf, char c, uint32_t *cursor, uint8_t attr);
void update_cursor(uint32_t position);
char keyboard_read(void);
int  keyboard_available(void);

extern uint32_t vga_cursor_pos;

void _start(void) {
    unsigned char *vga = (unsigned char *)VGA_BUFFER;
    uint32_t cursor = vga_cursor_pos;

    print_string(vga, "echo -- type lines, empty line to exit", &cursor);
    print_newline(vga, &cursor);

    while (1) {
        print_string(vga, "echo> ", &cursor);
        vga_cursor_pos = cursor;
        update_cursor(cursor);

        char line[128];
        int i = 0;
        while (1) {
            char c = keyboard_read();
            if (c == '\n') {
                print_newline(vga, &cursor);
                break;
            } else if (c == '\b') {
                if (i > 0) {
                    cursor -= 2;
                    print_char(vga, ' ', &cursor, 0x0B);
                    cursor -= 2;
                    i--;
                }
            } else if (i < 127) {
                print_char(vga, c, &cursor, 0x0F);
                line[i++] = c;
            }
        }
        line[i] = '\0';

        if (i == 0) break;

        print_string(vga, "  => \"", &cursor);
        print_string(vga, line, &cursor);
        print_string(vga, "\"", &cursor);
        print_newline(vga, &cursor);
    }

    vga_cursor_pos = cursor;
    update_cursor(cursor);
}
