#include "vga.h"
#include "io.h"

void update_cursor(uint32_t position) {
    uint16_t char_pos = position / 2;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(char_pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((char_pos >> 8) & 0xFF));
}

void clear_screen(unsigned char *vga_buffer) {
    const uint32_t vga_size = VGA_WIDTH * VGA_HEIGHT * 2;
    const uint8_t attribute = 0x0B;
    for (uint32_t i = 0; i < vga_size; i += 2) {
        vga_buffer[i] = ' ';
        vga_buffer[i + 1] = attribute;
    }
}

void print_char(unsigned char *vga_buf, char c, uint32_t *cursor, uint8_t attr) {
    vga_buf[*cursor] = c;
    vga_buf[*cursor + 1] = attr;
    *cursor += 2;
}

void print_string(unsigned char *vga_buf, const char *str, uint32_t *cursor) {
    while (*str) {
        vga_buf[*cursor] = *str++;
        vga_buf[*cursor + 1] = 0x0B;
        *cursor += 2;
    }
}

void print_newline(unsigned char *vga_buf, uint32_t *cursor) {
    uint32_t current_line = (*cursor / 2) / VGA_WIDTH;
    *cursor = (current_line + 1) * VGA_WIDTH * 2;
}