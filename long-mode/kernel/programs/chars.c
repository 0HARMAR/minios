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

static void print_hex2(uint8_t n) {
    static const char hex[] = "0123456789ABCDEF";
    putc(hex[n >> 4]);
    putc(hex[n & 0xF]);
}

void _start(void) {
    print("ASCII Character Set (0x20 - 0x7E)\n\n");

    print("    ");
    for (int col = 0; col < 16; col++) {
        print(" +");
        print_hex2((uint8_t)col);
    }
    print("\n");

    for (int row = 0; row < 6; row++) {
        print(" ");
        print_hex2((uint8_t)((row + 2) * 16));
        print(" ");

        for (int col = 1; col < 16; col++) {
            uint8_t code = (uint8_t)((row + 2) * 16 + col);
            if (code > 0x7E) break;
            print("  ");
            putc((char)code);
        }
        print("\n");
    }

    print("\n");
    exit(0);
}
