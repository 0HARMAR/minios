#include <stdint.h>
#include "../userlib.h"

__attribute__((noreturn))
void _start(void) {
    output(0, "hello, world from ring 3!\n", 26);

    while (1) {
        output(0, "type a key: ", 12);

        char c;
        input(1, &c, 1);

        output(0, "you typed: ", 11);
        output(0, &c, 1);
        output(0, "\n", 1);
    }
}
