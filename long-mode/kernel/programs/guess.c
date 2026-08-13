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
    rng_state = 0x1234;

    print("guess -- number guessing game (1-100)\n\n");

    int secret = rand_mod(100) + 1;
    int attempts = 0;
    char buf[16];

    while (1) {
        attempts++;
        itoa(attempts, buf);
        print("[#");
        print(buf);
        print("] your guess> ");

        char line[16];
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
            } else if (c >= '0' && c <= '9' && i < 15) {
                putc(c);
                line[i++] = c;
            }
        }
        line[i] = '\0';
        int guess = atoi_simple(line);

        if (guess == secret) {
            itoa(attempts, buf);
            print("correct! you got it in ");
            print(buf);
            print(" attempts.\n");
            break;
        } else if (guess < secret) {
            print("  too low, try higher");
        } else {
            print("  too high, try lower");
        }
        print("\n");
    }
    exit(0);
}
