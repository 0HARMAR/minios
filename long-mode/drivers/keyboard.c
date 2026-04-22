#include "keyboard.h"
#include "io.h"
#include "irq.h"
#include "pic.h"

#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64

static char key_buffer[256];
static int buffer_head = 0;
static int buffer_tail = 0;

static const char scan_to_ascii[128] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    0, 0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    0, 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* Extended mappings */
    [0x1C] = '\n',  /* Enter */
    [0x0E] = '\b',  /* Backspace */
    [0x39] = ' ',   /* Space */
};

static void keyboard_interrupt_handler(struct registers *r) {
    (void)r;
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    if (scancode < 128) {
        char c = scan_to_ascii[scancode];
        if (c != 0) {
            int next_head = (buffer_head + 1) % 256;
            if (next_head != buffer_tail) {
                key_buffer[buffer_head] = c;
                buffer_head = next_head;
            }
        }
    }
}

void keyboard_init(void) {
    buffer_head = 0;
    buffer_tail = 0;

    irq_register_handler(1, keyboard_interrupt_handler);

    pic_enable_irq(1);
}

int keyboard_available(void) {
    return buffer_head != buffer_tail;
}

char keyboard_read(void) {
    while (!keyboard_available());
    char c = key_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % 256;
    return c;
}