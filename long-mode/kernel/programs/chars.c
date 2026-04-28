#include <stdint.h>

#define VGA_BUFFER 0xB8000

void print_string(unsigned char *buf, const char *str, uint32_t *cursor);
void print_newline(unsigned char *buf, uint32_t *cursor);
void print_char(unsigned char *buf, char c, uint32_t *cursor, uint8_t attr);
void clear_screen(unsigned char *buf);
void update_cursor(uint32_t position);

extern uint32_t vga_cursor_pos;

static void print_hex2(uint8_t n, unsigned char *vga, uint32_t *cursor) {
    char hex[] = "0123456789ABCDEF";
    print_char(vga, hex[n >> 4], cursor, 0x0B);
    print_char(vga, hex[n & 0xF], cursor, 0x0B);
}

void _start(void) {
    unsigned char *vga = (unsigned char *)VGA_BUFFER;
    uint32_t cursor = 0;

    clear_screen(vga);

    print_string(vga, "ASCII Character Set (0x20 - 0x7E)", &cursor);
    print_newline(vga, &cursor);
    print_newline(vga, &cursor);

    print_string(vga, "    ", &cursor);
    for (int col = 0; col < 16; col++) {
        print_string(vga, " +", &cursor);
        print_hex2(col, vga, &cursor);
    }
    print_newline(vga, &cursor);

    for (int row = 0; row < 6; row++) {
        print_string(vga, " ", &cursor);
        print_hex2((uint8_t)((row + 2) * 16), vga, &cursor);
        print_string(vga, " ", &cursor);

        for (int col = 1; col < 16; col++) {
            uint8_t code = (uint8_t)((row + 2) * 16 + col);
            if (code > 0x7E) break;
            print_string(vga, "  ", &cursor);
            print_char(vga, (char)code, &cursor, 0x0F);
        }
        print_newline(vga, &cursor);
    }

    print_newline(vga, &cursor);

    vga_cursor_pos = cursor;
    update_cursor(cursor);
}
