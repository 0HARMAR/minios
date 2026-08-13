#include <stdint.h>
#include "../userlib.h"

static uint64_t slen(const char *s)
{
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *s)
{
    output(0, s, slen(s));
}

static char putc_buf;
static void putc(char c)
{
    putc_buf = c;
    output(0, &putc_buf, 1);
}

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

static int read_num(void) {
    char buf[16];
    int i = 0;
    while (1) {
        char c;
        if (input(1, &c, 1) < 0)
            continue;   /* keyboard owned by another process */
        if (c == '\n') {
            print("\n");
            break;
        } else if (c == '\b') {
            if (i > 0) {
                print("\b \b");
                i--;
            }
        } else if ((c >= '0' && c <= '9') || (i == 0 && c == '-')) {
            if (i < 15) {
                putc(c);
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

static char read_op(void) {
    while (1) {
        char c;
        if (input(1, &c, 1) < 0)
            continue;   /* keyboard owned by another process */
        if (c == '+' || c == '-' || c == '*' || c == '/') {
            putc(c);
            return c;
        }
        if (c == 'q') return 'q';
    }
}

void _start(void) {
    print("calc -- 4-function calculator (q to quit)\n");

    while (1) {
        print("\n");
        print("num1> ");
        int a = read_num();

        print("  op> ");
        char op = read_op();
        if (op == 'q') break;
        print("\n");

        print("num2> ");
        int b = read_num();

        int result;
        char buf[16];
        switch (op) {
            case '+': result = a + b; break;
            case '-': result = a - b; break;
            case '*': result = a * b; break;
            case '/': result = (b != 0) ? a / b : 0; break;
            default:  result = 0; break;
        }

        print("  ");
        itoa(a, buf);
        print(buf);
        print(" ");
        putc(op);
        print(" ");
        itoa(b, buf);
        print(buf);
        print(" = ");
        itoa(result, buf);
        print(buf);
        print("\n");
    }

    print("bye!\n");
    exit(0);
}
