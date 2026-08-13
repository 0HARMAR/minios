#include <stdint.h>
#include "../userlib.h"

static uint64_t strlen(const char *s)
{
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *s)
{
    output(0, s, strlen(s));
}

static char putc_buf;
static void putc(char c)
{
    putc_buf = c;
    output(0, &putc_buf, 1);
}

static int strncmp(const char *a, const char *b, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)(a[i] - b[i]);
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

static uint64_t utoa(uint64_t n, char *buf)
{
    if (n == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    char tmp[21];
    uint32_t i = 0;
    while (n > 0) {
        tmp[i++] = '0' + (char)(n % 10);
        n /= 10;
    }
    uint32_t len = i;
    for (uint32_t j = 0; j < len; j++)
        buf[j] = tmp[len - 1 - j];
    buf[len] = '\0';
    return len;
}

static char *next_word(char **p)
{
    while (**p == ' ') (*p)++;
    if (**p == '\0') return 0;
    char *word = *p;
    while (**p && **p != ' ') (*p)++;
    if (**p == ' ') { **p = '\0'; (*p)++; }
    return word;
}

__attribute__((noreturn))
void _start(void)
{
    print("ush v0.1\n");

    char cmd[128];
    uint32_t idx;

    while (1) {
        print("$ ");
        idx = 0;

        for (;;) {
            char c;
            if (input(1, &c, 1) < 0) {
                continue;
            }

            if (c == '\n') {
                putc('\n');
                break;
            }
            if (c == '\b' && idx > 0) {
                print("\b \b");
                idx--;
                continue;
            }
            if (c == '\b')
                continue;
            if (idx < 127) {
                putc(c);
                cmd[idx++] = c;
            }
        }
        cmd[idx] = '\0';

        char *p = cmd;
        char *cmd_name = next_word(&p);

        if (cmd_name == 0) {
            /* empty line */
        } else if (strncmp(cmd_name, "run", 3) == 0 && cmd_name[3] == '\0') {
            char *prog = next_word(&p);
            if (prog == 0) {
                print("usage: run <prog>\n");
            } else {
                print("spawn ");
                print(prog);
                print(": ");
                int64_t pid = crtproc(prog, 0);
                if (pid < 0) {
                    print("err=");
                    char buf[8];
                    utoa((uint64_t)(-pid), buf);
                    print(buf);
                    print("\n");
                } else {
                    print("pid ");
                    char buf[8];
                    utoa((uint64_t)pid, buf);
                    print(buf);
                    print("\n");
                }
            }
        } else if (strncmp(cmd_name, "help", 4) == 0 && cmd_name[4] == '\0') {
            print("run <prog>  spawn a process\n");
            print("help        this message\n");
        } else {
            print("?: ");
            print(cmd_name);
            print("\n");
        }
    }
}
