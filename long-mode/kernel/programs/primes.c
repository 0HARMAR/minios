#include <stdint.h>

#define VGA_BUFFER 0xB8000

void print_string(unsigned char *buf, const char *str, uint32_t *cursor);
void print_newline(unsigned char *buf, uint32_t *cursor);
void print_char(unsigned char *buf, char c, uint32_t *cursor, uint8_t attr);
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

    print_string(vga, "primes -- prime numbers up to 500", &cursor);
    print_newline(vga, &cursor);
    print_newline(vga, &cursor);

    int count = 0;
    char buf[16];

    for (int n = 2; n <= 500; n++) {
        int is_prime = 1;
        for (int d = 2; d * d <= n; d++) {
            if (n % d == 0) { is_prime = 0; break; }
        }
        if (is_prime) {
            itoa(n, buf);
            print_string(vga, buf, &cursor);
            count++;
            if (count % 10 == 0) {
                print_newline(vga, &cursor);
            } else {
                print_string(vga, "  ", &cursor);
            }
        }
    }

    print_newline(vga, &cursor);
    print_newline(vga, &cursor);
    print_string(vga, "found ", &cursor);
    itoa(count, buf);
    print_string(vga, buf, &cursor);
    print_string(vga, " primes", &cursor);
    print_newline(vga, &cursor);

    vga_cursor_pos = cursor;
    update_cursor(cursor);
}
