#include <stdint.h>

void outb(uint16_t port, uint8_t value);
uint8_t inb(uint16_t port);

void update_cursor(uint32_t position) {
    // calculate char pos (2 byte per char)
    uint16_t char_pos = position / 2;

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(char_pos & 0xFF));

    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((char_pos >> 8) & 0xFF));
}

void clear_screen(unsigned char *vga_buffer) {
    const uint32_t vga_size = 80 * 25 * 2;
    const uint8_t attribute = 0x0A;

    for (uint32_t i = 0; i < vga_size; i += 2) {
        vga_buffer[i] = ' ';
        vga_buffer[i + 1] = attribute;
    }
}

void print_string(unsigned char *vga_buf, const char *str, uint32_t *cursor) {
    while (*str) {
        vga_buf[*cursor] = *str++;
        vga_buf[*cursor + 1] = 0x0A;
        *cursor += 2;
    }
}

void kernel_main() {
    unsigned char *vga_buffer = (unsigned char*)0xB8000;

    clear_screen(vga_buffer);

    uint32_t cursor_pos = 0;
    print_string(vga_buffer, "MyOS v0.1 Shell", &cursor_pos);
    cursor_pos = 80 * 2 * 2; // start with second line
    print_string(vga_buffer, "$", &cursor_pos);
    update_cursor(cursor_pos);

    // scan code to char mapping
    const char scan_to_ascii[256] = {
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
        [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
        [0x0A] = '9', [0x0B] = '0', [0x10] = 'q', [0x11] = 'w',
        [0x12] = 'e', [0x13] = 'r', [0x14] = 't', [0x15] = 'y',
        [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
        [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
        [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
        [0x26] = 'l', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c',
        [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
        [0x39] = ' ',  // 空格
        [0x1C] = '\n', // 回车
        [0x0E] = '\b'  // 退格
    };

    // Shell cmd buffer
    char cmd_buffer[128] = {0};
    uint32_t cmd_index = 0;

    // Shell status
    enum { INPUT_MODE, EXEC_MODE } shell_state = INPUT_MODE;

    // keyboard status tarce
    uint32_t last_process_time = 0;

    while (1) {
        if (inb(0x64) & 0x01) {
            uint8_t scancode = inb(0x60);

        // only process key down 
        if (scancode < 0x80) {
                char c = scan_to_ascii[scancode];

            if (shell_state == INPUT_MODE) {
                // process code that could print
                if (c > 0 && c != '\n' && c != '\b') {
                    if (cmd_index < sizeof(cmd_buffer) - 1) {
                        vga_buffer[cursor_pos] = c;
                        vga_buffer[cursor_pos + 1] = 0x0A;
                        cursor_pos += 2;
                        cmd_buffer[cmd_index ++] = c;
                    }
                }
                else if (c == '\n') {
                    if (cmd_index > 0) {
                        cmd_buffer[cmd_index] = '\0';
                        print_string(vga_buffer, "\nExecuting: ", &cursor_pos);
                        print_string(vga_buffer, cmd_buffer, &cursor_pos);
                        print_string(vga_buffer, " ...", &cursor_pos);

                        uint32_t current_line = (cursor_pos / 2) /80;
                        cursor_pos = (current_line + 1) * 80 * 2;

                        shell_state = EXEC_MODE;
                    }
                }
                else if (c == '\b' && cursor_pos > 0) {
                    cursor_pos -= 2;
                    vga_buffer[cursor_pos] = ' ';
                    vga_buffer[cursor_pos + 1] = 0x0A;
                    cmd_buffer[--cmd_index] = '\0';
                }
            }
            else if (shell_state == EXEC_MODE) {
                if (c == '\n') {
                    // show new prompt
                    print_string(vga_buffer, "\n\r$ ", &cursor_pos);
                    cmd_index = 0;
                    cmd_buffer[0] = '\0';
                    shell_state = INPUT_MODE;
                }
            }

            update_cursor(cursor_pos);
        }
        }
        // simple delay
        for (volatile int i = 0; i < 1000; i++);
    }
}

void outb(uint16_t port, uint8_t value) {
    if (port < 256) {
        asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
    } else {
        asm volatile("outb %0, %%dx" : : "a"(value), "d"(port));
    }
}

uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port));
    return ret;
}