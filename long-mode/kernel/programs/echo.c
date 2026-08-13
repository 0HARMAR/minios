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

void _start(void) {
    print("echo -- type lines, empty line to exit\n");

    while (1) {
        print("echo> ");

        char line[128];
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
            } else if (i < 127) {
                putc(c);
                line[i++] = c;
            }
        }
        line[i] = '\0';

        if (i == 0) break;

        print("  => \"");
        print(line);
        print("\"\n");
    }
    exit(0);
}
