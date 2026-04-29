#include "lib.h"

void *memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *s, int c, uint32_t n) {
    uint8_t *p = s;
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

int32_t strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

uint32_t strlen(const char *s) {
    uint32_t n = 0;
    while (*s++) n++;
    return n;
}

void utoa(uint32_t n, char *buf) {
    uint32_t i = 0;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    buf[i] = '\0';
    for (uint32_t j = 0; j < i / 2; j++) {
        char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
}
