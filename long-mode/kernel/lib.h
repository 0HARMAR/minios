#ifndef LIB_H
#define LIB_H

#include <stdint.h>

void *memcpy(void *dst, const void *src, uint32_t n);
void *memset(void *s, int c, uint32_t n);
int32_t strcmp(const char *a, const char *b);
uint32_t strlen(const char *s);
void utoa(uint32_t n, char *buf);

#endif
