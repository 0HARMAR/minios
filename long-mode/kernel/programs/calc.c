#include <stdint.h>

#define VGA_BUFFER 0xB8000

void print_string(unsigned char *buf, const char *str, uint32_t *cursor);
void print_newline(unsigned char *buf, uint32_t *cursor);
void print_char(unsigned char *buf, char c, uint32_t *cursor, uint8_t attr);
void update_cursor(uint32_t position);
char keyboard_read(void);

extern uint32_t vga_cursor_pos;

static void itoa(int n, char *buf) {
    int i = 0;
    if (n < 0) { n = -n; buf[i++] = '-'; }
    int start = i;
    if (n == 0) { buf[i++] = '0'; }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    int end = i - 1;
    while (start < end) {
        char t = buf[start];
        buf[start] = buf[end];
        buf[end] = t;
        start++; end--;
    }
    buf[i] = '\0';
}

static int read_num(unsigned char *vga, uint32_t *cursor) {
    char buf[16];
    int i = 0;
    while (1) {
        char c = keyboard_read();
        if (c == '\n') {
            print_newline(vga, cursor);
            break;
        } else if (c == '\b') {
            if (i > 0) {
                *cursor -= 2;
                print_char(vga, ' ', cursor, 0x0B);
                *cursor -= 2;
                i--;
            }
        } else if ((c >= '0' && c <= '9') || (i == 0 && c == '-')) {
            if (i < 15) {
                print_char(vga, c, cursor, 0x0F);
                buf[i++] = c;
            }
        }
    }
    buf[i] = '\0';

    int n = 0, sign = 1, j = 0;
    if (buf[0] == '-') { sign = -1; j = 1; }
    while (buf[j]) { n = n * 10 + (buf[j] - '0'); j++; }
    return n * sign;
}

static char read_op(unsigned char *vga, uint32_t *cursor) {
    while (1) {
        char c = keyboard_read();
        if (c == '+' || c == '-' || c == '*' || c == '/') {
            print_char(vga, c, cursor, 0x0F);
            return c;
        }
        if (c == 'q') return 'q';
    }
}

void _start(void) {
    unsigned char *vga = (unsigned char *)VGA_BUFFER;
    uint32_t cursor = vga_cursor_pos;

    print_string(vga, "calc -- 4-function calculator (q to quit)", &cursor);
    print_newline(vga, &cursor);

    while (1) {
        print_newline(vga, &cursor);
        print_string(vga, "num1> ", &cursor);
        vga_cursor_pos = cursor;
        update_cursor(cursor);
        int a = read_num(vga, &cursor);

        print_string(vga, "  op> ", &cursor);
        vga_cursor_pos = cursor;
        update_cursor(cursor);
        char op = read_op(vga, &cursor);
        if (op == 'q') break;
        print_newline(vga, &cursor);

        print_string(vga, "num2> ", &cursor);
        vga_cursor_pos = cursor;
        update_cursor(cursor);
        int b = read_num(vga, &cursor);

        int result;
        char buf[16];
        switch (op) {
            case '+': result = a + b; break;
            case '-': result = a - b; break;
            case '*': result = a * b; break;
            case '/': result = (b != 0) ? a / b : 0; break;
            default:  result = 0; break;
        }

        itoa(a, buf);
        print_string(vga, "  ", &cursor);
        print_string(vga, buf, &cursor);

        char op_str[2] = {op, '\0'};
        op_str[0] = op;
        print_string(vga, " ", &cursor);
        print_string(vga, op_str, &cursor);
        print_string(vga, " ", &cursor);

        itoa(b, buf);
        print_string(vga, buf, &cursor);
        print_string(vga, " = ", &cursor);

        itoa(result, buf);
        print_string(vga, buf, &cursor);
        print_newline(vga, &cursor);
    }

    print_string(vga, "bye!", &cursor);
    print_newline(vga, &cursor);

    vga_cursor_pos = cursor;
    update_cursor(cursor);
}
