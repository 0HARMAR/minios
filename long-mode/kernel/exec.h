#ifndef EXEC_H
#define EXEC_H

#include <stdint.h>
#include "aout.h"

#define MAX_PROGS      16

/* Programs are loaded here at runtime */
#define PROGRAMS_BASE  0x200000

extern uint32_t vga_cursor_pos;

void exec_init(void);
int  exec(const char *name);   /* 0 on success, -1 if not found */

#endif
