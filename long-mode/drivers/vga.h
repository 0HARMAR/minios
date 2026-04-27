#ifndef VGA_H
#define VGA_H

#include <stdint.h>

#define VGA_BUFFER 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

void update_cursor(uint32_t position);
void clear_screen(unsigned char *vga_buffer);
void print_char(unsigned char *vga_buf, char c, uint32_t *cursor, uint8_t attr);
void print_string(unsigned char *vga_buf, const char *str, uint32_t *cursor);
void scroll_screen(unsigned char *vga_buffer);
void print_newline(unsigned char *vga_buf, uint32_t *cursor);

#endif