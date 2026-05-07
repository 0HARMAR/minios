#include <stdint.h>
#include <stddef.h>
#include "io.h"
#include "vga.h"
#include "interrupts.h"
#include "keyboard.h"
#include "exec.h"
#include "fs.h"
#include "lib.h"
#include "timer.h"

/* user-mode entry (arch/user.S) */
extern uint64_t gdt64[];
extern uint8_t  tss[104];
extern void     enter_user_mode(void *entry, void *stack);
extern void     user_program(void);

__attribute__((section(".data"))) uint32_t vga_cursor_pos;

static char *next_word(char **p) {
    while (**p == ' ') (*p)++;
    if (**p == '\0') return NULL;
    char *word = *p;
    while (**p && **p != ' ') (*p)++;
    if (**p == ' ') { **p = '\0'; (*p)++; }
    return word;
}

static void tss_init(void) {
    /* zero TSS */
    for (int i = 0; i < 104; i++)
        tss[i] = 0;

    /* RSP0 = kernel stack top (offset 4) */
    *(uint64_t *)(tss + 4) = 0x90000;

    /* IOPB offset past TSS = no I/O bitmap (offset 100) */
    *(uint16_t *)(tss + 100) = 104;

    /* fill TSS descriptor in GDT[5] and GDT[6] */
    uint64_t addr  = (uint64_t)tss;
    uint64_t limit = 103;  /* sizeof(TSS) - 1 */

    gdt64[5] = (limit & 0xFFFFULL)
             | ((addr & 0xFFFFULL) << 16)
             | (((addr >> 16) & 0xFFULL) << 32)
             | (0x89ULL << 40)
             | (((limit >> 16) & 0xFULL) << 48)
             | (((addr >> 24) & 0xFFULL) << 56);

    gdt64[6] = (addr >> 32) & 0xFFFFFFFFULL;
}

static void fs_print_line(const char *s) {
    unsigned char *vga = (unsigned char *)VGA_BUFFER;
    print_string(vga, s, &vga_cursor_pos);
    print_newline(vga, &vga_cursor_pos);
}

void kernel_main() {
    unsigned char *vga_buffer = (unsigned char*)VGA_BUFFER;

    clear_screen(vga_buffer);

    vga_cursor_pos = 0;
    print_string(vga_buffer, "MiniOS 64-bit Shell", &vga_cursor_pos);
    vga_cursor_pos = 80 * 2 * 2;

    exec_init();
    interrupts_init();
    keyboard_init();
    timer_init();
    fs_init();
    tss_init();

    if (fs_is_ready())
        print_string(vga_buffer, "[fs ready]", &vga_cursor_pos);
    else
        print_string(vga_buffer, "[fs not ready - type mkfs]", &vga_cursor_pos);

    print_string(vga_buffer, "$ ", &vga_cursor_pos);
    update_cursor(vga_cursor_pos);

    char cmd_buffer[128] = {0};
    uint32_t cmd_index = 0;

    while (1) {
        if (keyboard_available()) {
            char c = keyboard_read();
            if (c != '\n' && c != '\b') {
                if (cmd_index < sizeof(cmd_buffer) - 1) {
                    print_char(vga_buffer, c, &vga_cursor_pos, 0x0F);
                    cmd_buffer[cmd_index++] = c;
                }
            } else if (c == '\n') {
                cmd_buffer[cmd_index] = '\0';
                print_newline(vga_buffer, &vga_cursor_pos);

                if (cmd_index > 0) {
                    char *p = cmd_buffer;
                    char *cmd = next_word(&p);

                    /* ── filesystem commands ── */
                    if (cmd && strcmp(cmd, "mkfs") == 0) {
                        fs_mkfs();
                        print_string(vga_buffer, "filesystem formatted", &vga_cursor_pos);
                    } else if (cmd && strcmp(cmd, "ls") == 0) {
                        fs_list(fs_print_line);
                    } else if (cmd && strcmp(cmd, "touch") == 0) {
                        char *name = next_word(&p);
                        if (name) {
                            int32_t r = fs_create(name);
                            if (r == -2)
                                print_string(vga_buffer, "file exists", &vga_cursor_pos);
                            else if (r < 0)
                                print_string(vga_buffer, "create failed", &vga_cursor_pos);
                            else {
                                print_string(vga_buffer, "created ", &vga_cursor_pos);
                                print_string(vga_buffer, name, &vga_cursor_pos);
                            }
                        }
                    } else if (cmd && strcmp(cmd, "write") == 0) {
                        char *name = next_word(&p);
                        if (name && *p) {
                            int32_t ino = fs_find(name);
                            if (ino < 0) {
                                print_string(vga_buffer, "file not found: ", &vga_cursor_pos);
                                print_string(vga_buffer, name, &vga_cursor_pos);
                            } else {
                                int32_t n = fs_write(ino, (uint8_t *)p, strlen(p));
                                if (n < 0)
                                    print_string(vga_buffer, "write failed", &vga_cursor_pos);
                                else {
                                    print_string(vga_buffer, "wrote ", &vga_cursor_pos);
                                    char num[16];
                                    utoa((uint32_t)n, num);
                                    print_string(vga_buffer, num, &vga_cursor_pos);
                                    print_string(vga_buffer, " bytes", &vga_cursor_pos);
                                }
                            }
                        }
                    } else if (cmd && strcmp(cmd, "cat") == 0) {
                        char *name = next_word(&p);
                        if (name) {
                            int32_t ino = fs_find(name);
                            if (ino < 0) {
                                print_string(vga_buffer, "file not found: ", &vga_cursor_pos);
                                print_string(vga_buffer, name, &vga_cursor_pos);
                            } else {
                                char buf[256];
                                int32_t n = fs_read(ino, (uint8_t *)buf, sizeof(buf) - 1);
                                if (n < 0)
                                    print_string(vga_buffer, "read failed", &vga_cursor_pos);
                                else if (n == 0)
                                    ;
                                else {
                                    buf[n] = '\0';
                                    print_string(vga_buffer, buf, &vga_cursor_pos);
                                }
                            }
                        }
                    } else if (cmd && strcmp(cmd, "uptime") == 0) {
                        uint64_t ticks = timer_get_ticks();
                        print_string(vga_buffer, "ticks: ", &vga_cursor_pos);
                        char num[32];
                        utoa((uint32_t)(ticks % 1000000), num);
                        print_string(vga_buffer, num, &vga_cursor_pos);
                    } else if (cmd && strcmp(cmd, "sleep") == 0) {
                        char *arg = next_word(&p);
                        if (arg) {
                            uint32_t n = 0;
                            for (char *d = arg; *d; d++) n = n * 10 + (*d - '0');
                            timer_wait(n);
                        }
                    } else if (cmd && strcmp(cmd, "help") == 0) {
                        print_string(vga_buffer, "mkfs touch write cat ls uptime sleep help user", &vga_cursor_pos);
                    } else if (cmd && strcmp(cmd, "user") == 0) {
                        /* switch to ring 3 — never returns */
                        enter_user_mode((void *)user_program, (void *)0x400000);
                    } else if (exec(cmd_buffer) != 0) {
                        print_string(vga_buffer, "not found: ", &vga_cursor_pos);
                        print_string(vga_buffer, cmd_buffer, &vga_cursor_pos);
                    }
                }

                print_newline(vga_buffer, &vga_cursor_pos);
                print_string(vga_buffer, "$ ", &vga_cursor_pos);
                cmd_index = 0;
            } else if (c == '\b' && cmd_index > 0) {
                vga_cursor_pos -= 2;
                print_char(vga_buffer, ' ', &vga_cursor_pos, 0x0B);
                vga_cursor_pos -= 2;
                cmd_buffer[--cmd_index] = '\0';
            }
            update_cursor(vga_cursor_pos);
        }
    }
}
