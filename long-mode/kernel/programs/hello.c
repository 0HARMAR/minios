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

void _start(void) {
    print("Hello from program!\n");
    print("  -> ran via MINI exec format\n");
    exit(0);
}
