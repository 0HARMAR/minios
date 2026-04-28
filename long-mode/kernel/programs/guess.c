#include <stdint.h>

#define VGA_BUFFER 0xB8000

void print_string(unsigned char *buf, const char *str, uint32_t *cursor);
void print_newline(unsigned char *buf, uint32_t *cursor);
void print_char(unsigned char *buf, char c, uint32_t *cursor, uint8_t attr);
void update_cursor(uint32_t position);
char keyboard_read(void);

extern uint32_t vga_cursor_pos;

static uint32_t rng_state;

static int rand_mod(int n) {
    rng_state = rng_state * 1103515245 + 12345;
    return (int)((rng_state >> 16) % (uint32_t)n);
}

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

static int atoi_simple(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

void _start(void) {
    unsigned char *vga = (unsigned char *)VGA_BUFFER;
    uint32_t cursor = vga_cursor_pos;

    rng_state = vga_cursor_pos; /* seed from cursor position */

    print_string(vga, "guess -- number guessing game (1-100)", &cursor);
    print_newline(vga, &cursor);
    print_newline(vga, &cursor);

    int secret = rand_mod(100) + 1;
    int attempts = 0;
    char buf[16];

    while (1) {
        attempts++;
        itoa(attempts, buf);
        print_string(vga, "[#", &cursor);
        print_string(vga, buf, &cursor);
        print_string(vga, "] your guess> ", &cursor);
        vga_cursor_pos = cursor;
        update_cursor(cursor);

        char line[16];
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
            } else if (c >= '0' && c <= '9' && i < 15) {
                print_char(vga, c, &cursor, 0x0F);
                line[i++] = c;
            }
        }
        line[i] = '\0';
        int guess = atoi_simple(line);

        if (guess == secret) {
            itoa(attempts, buf);
            print_string(vga, "correct! you got it in ", &cursor);
            print_string(vga, buf, &cursor);
            print_string(vga, " attempts.", &cursor);
            print_newline(vga, &cursor);
            break;
        } else if (guess < secret) {
            print_string(vga, "  too low, try higher", &cursor);
        } else {
            print_string(vga, "  too high, try lower", &cursor);
        }
        print_newline(vga, &cursor);
    }

    vga_cursor_pos = cursor;
    update_cursor(cursor);
}
