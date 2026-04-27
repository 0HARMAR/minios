#include <stdint.h>

#define VGA_BUFFER 0xB8000

void print_string(unsigned char *buf, const char *str, uint32_t *cursor);
void print_newline(unsigned char *buf, uint32_t *cursor);
void update_cursor(uint32_t position);

extern uint32_t vga_cursor_pos;

void _start(void) {
    unsigned char *vga = (unsigned char *)VGA_BUFFER;
    uint32_t cursor = vga_cursor_pos;

    print_string(vga, "Hello from program!", &cursor);
    print_newline(vga, &cursor);
    print_string(vga, "  -> ran via MINI exec format", &cursor);
    print_newline(vga, &cursor);

    vga_cursor_pos = cursor;
    update_cursor(cursor);
}
