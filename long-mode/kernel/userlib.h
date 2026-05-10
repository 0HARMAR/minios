#ifndef USERLIB_H
#define USERLIB_H

#include <stdint.h>

static inline int64_t output(uint8_t dd, const void *buf, uint64_t len) {
    int64_t ret;
    asm volatile(
        "movq $1, %%rax\n"
        "int  $0x80\n"
        : "=a"(ret)
        : "D"((uint64_t)dd), "S"((uint64_t)buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t input(uint8_t dd, void *buf, uint64_t len) {
    int64_t ret;
    asm volatile(
        "movq $2, %%rax\n"
        "int  $0x80\n"
        : "=a"(ret)
        : "D"((uint64_t)dd), "S"((uint64_t)buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#endif
