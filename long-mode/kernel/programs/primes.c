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
    print("primes -- prime numbers up to 500\n");
    print("\n");

    int count = 0;
    char buf[16];

    for (int n = 2; n <= 500; n++) {
        int is_prime = 1;
        for (int d = 2; d * d <= n; d++) {
            if (n % d == 0) { is_prime = 0; break; }
        }
        if (is_prime) {
            itoa(n, buf);
            print(buf);
            count++;
            if (count % 10 == 0) {
                print("\n");
            } else {
                print("  ");
            }
        }
    }

    print("\n\nfound ");
    itoa(count, buf);
    print(buf);
    print(" primes\n");
    exit(0);
}
