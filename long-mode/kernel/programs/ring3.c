#include <stdint.h>
#include "../userlib.h"

void _start(void) {
    output(2, "hello, world from ring 3!\n", 26);
    exit(0);
}
