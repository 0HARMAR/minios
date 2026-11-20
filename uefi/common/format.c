/*
 * format.c - tiny freestanding printf for the loader and the kernel.
 *
 * Supported: %s %c %d %i %u %x %X %p, the 'l' and 'll' length modifiers,
 * a leading zero flag and a decimal field width.  No floating point.
 */
#include "format.h"

struct outbuf {
    char *buf;
    unsigned long cap;
    unsigned long len;
};

static void out_ch(struct outbuf *o, char c)
{
    if (o->cap != 0 && o->len < o->cap - 1)
        o->buf[o->len] = c;
    o->len++;
}

static void out_uint(struct outbuf *o, unsigned long long value, unsigned base,
                     int upper, unsigned long width, int zero, int prefix)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int n = 0;
    unsigned long total;

    if (value == 0)
        tmp[n++] = '0';
    while (value != 0) {
        tmp[n++] = digits[value % base];
        value /= base;
    }

    total = (unsigned long)n + (prefix ? 2u : 0u);
    if (prefix && zero) {
        out_ch(o, '0');
        out_ch(o, upper ? 'X' : 'x');
    }
    while (total < width) {
        out_ch(o, zero ? '0' : ' ');
        total++;
    }
    if (prefix && !zero) {
        out_ch(o, '0');
        out_ch(o, upper ? 'X' : 'x');
    }
    while (n > 0)
        out_ch(o, tmp[--n]);
}

unsigned long mini_vsnprintf(char *out, unsigned long cap, const char *fmt, va_list ap)
{
    struct outbuf o;

    o.buf = out;
    o.cap = cap;
    o.len = 0;

    while (*fmt != '\0') {
        unsigned long width = 0;
        int zero = 0;
        int lng = 0;
        char conv;

        if (*fmt != '%') {
            out_ch(&o, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            out_ch(&o, '%');
            fmt++;
            continue;
        }
        if (*fmt == '0') {
            zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (unsigned long)(*fmt - '0');
            fmt++;
        }
        while (*fmt == 'l') {
            lng++;
            fmt++;
        }
        conv = *fmt;
        if (conv == '\0')
            break;
        fmt++;

        switch (conv) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == (const char *)0)
                s = "(null)";
            while (*s != '\0')
                out_ch(&o, *s++);
            break;
        }
        case 'c':
            out_ch(&o, (char)va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            long long sv;
            unsigned long long uv;
            int neg;

            if (lng >= 2)
                sv = va_arg(ap, long long);
            else if (lng == 1)
                sv = va_arg(ap, long);
            else
                sv = va_arg(ap, int);
            neg = sv < 0;
            uv = neg ? (unsigned long long)(-(sv + 1)) + 1ULL
                     : (unsigned long long)sv;
            if (neg)
                out_ch(&o, '-');
            out_uint(&o, uv, 10, 0, width, zero, 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long long uv;
            unsigned base = (conv == 'u') ? 10u : 16u;

            if (lng >= 2)
                uv = va_arg(ap, unsigned long long);
            else if (lng == 1)
                uv = va_arg(ap, unsigned long);
            else
                uv = va_arg(ap, unsigned int);
            out_uint(&o, uv, base, conv == 'X', width, zero, 0);
            break;
        }
        case 'p': {
            unsigned long long pv =
                (unsigned long long)(unsigned long)va_arg(ap, void *);
            out_uint(&o, pv, 16, 0, 16, 1, 1);
            break;
        }
        default:
            out_ch(&o, '%');
            out_ch(&o, conv);
            break;
        }
    }

    if (o.cap != 0) {
        unsigned long end = o.len < o.cap ? o.len : o.cap - 1;
        o.buf[end] = '\0';
    }
    return o.len;
}

unsigned long mini_snprintf(char *out, unsigned long cap, const char *fmt, ...)
{
    va_list ap;
    unsigned long ret;

    va_start(ap, fmt);
    ret = mini_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return ret;
}
