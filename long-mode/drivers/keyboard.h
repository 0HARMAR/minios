#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);
char keyboard_read(void);
int keyboard_available(void);
int keyboard_acquire(int pid);
void keyboard_release(int pid);

#endif