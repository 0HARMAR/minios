#include <stdint.h>

#define VGA_BUFFER 0xB8000

void print_string(unsigned char *buf, const char *str, uint32_t *cursor);
void print_newline(unsigned char *buf, uint32_t *cursor);
void update_cursor(uint32_t position);

extern uint32_t vga_cursor_pos;

static void itoa(int n, char *buf) {
    int i = 0;
    if (n == 0) { buf[i++] = '0'; }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
    buf[i] = '\0';
}

void _start(void) {
    unsigned char *vga = (unsigned char *)VGA_BUFFER;
    uint32_t cursor = vga_cursor_pos;

    print_string(vga, "fib -- first 30 Fibonacci numbers", &cursor);
    print_newline(vga, &cursor);
    print_newline(vga, &cursor);

    int a = 0, b = 1;
    char buf[16];

    for (int i = 0; i < 30; i++) {
        itoa(i, buf);
        print_string(vga, "F(", &cursor);
        print_string(vga, buf, &cursor);
        print_string(vga, ") = ", &cursor);

        itoa(a, buf);
        print_string(vga, buf, &cursor);
        print_newline(vga, &cursor);

        int next = a + b;
        a = b;
        b = next;
    }

    vga_cursor_pos = cursor;
    update_cursor(cursor);
}
