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
    print("fib -- first 30 Fibonacci numbers\n");
    print("\n");

    int a = 0, b = 1;
    char buf[16];

    for (int i = 0; i < 30; i++) {
        print("F(");
        itoa(i, buf);
        print(buf);
        print(") = ");

        itoa(a, buf);
        print(buf);
        print("\n");

        int next = a + b;
        a = b;
        b = next;
    }
    exit(0);
}
