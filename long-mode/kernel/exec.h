#ifndef EXEC_H
#define EXEC_H

#include <stdint.h>

#define EXEC_MAGIC     0x4D494E49  /* "MINI" little-endian */
#define EXEC_NAME_MAX  16
#define MAX_PROGS      16

/* 32-byte header before each program's code */
typedef struct __attribute__((packed)) {
    uint32_t magic;                    /* EXEC_MAGIC */
    char     name[EXEC_NAME_MAX];      /* null-terminated */
    uint32_t code_size;                /* code bytes after header */
    uint64_t entry;                    /* absolute entry address */
} exec_header_t;

/* Programs are loaded here at runtime */
#define PROGRAMS_BASE  0x100000

extern uint32_t vga_cursor_pos;

void exec_init(void);
int  exec(const char *name);   /* 0 on success, -1 if not found */

#endif
