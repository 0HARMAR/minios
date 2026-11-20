#ifndef MINIOS_FORMAT_H
#define MINIOS_FORMAT_H

#include <stdarg.h>

unsigned long mini_vsnprintf(char *out, unsigned long cap, const char *fmt, va_list ap);
unsigned long mini_snprintf(char *out, unsigned long cap, const char *fmt, ...);

#endif /* MINIOS_FORMAT_H */
