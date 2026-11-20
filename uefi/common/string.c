/*
 * string.c - the handful of libc routines a freestanding build can emit calls
 * to.  Built with -fno-builtin so these loops do not turn into calls to
 * themselves.
 */
#include "string.h"

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n-- != 0)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d < s) {
        while (n-- != 0)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n-- != 0)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;

    while (n-- != 0)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while (n-- != 0) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

unsigned long strlen(const char *s)
{
    const char *p = s;

    while (*p != '\0')
        p++;
    return (unsigned long)(p - s);
}
