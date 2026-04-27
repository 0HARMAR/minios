#include "exec.h"

/* name -> entry_address table, built once at init */
static struct {
    char     name[EXEC_NAME_MAX];
    void   (*entry)(void);
} prog_table[MAX_PROGS];

static int prog_count;

/* _kernel_load_end: first byte after .data (where programs follow on disk).
 * _kernel_end:       first byte after .bss (BSS needs runtime zeroing). */
extern uint8_t _kernel_load_end[];
extern uint8_t _kernel_end[];

void exec_init(void) {
    uint8_t       *src = _kernel_load_end;
    uint8_t       *dst = (uint8_t *)PROGRAMS_BASE;
    exec_header_t *hdr;
    uint32_t       total = 0;

    /* Phase 1: copy programs out of the BSS-overlap region before
     * BSS zeroing destroys them.  The programs binary was loaded
     * right after the kernel on disk, which lands on _kernel_load_end
     * — the same memory the linker assigned to .bss. */
    while (1) {
        hdr = (exec_header_t *)src;
        if (hdr->magic != EXEC_MAGIC)
            break;
        uint32_t bytes = sizeof(exec_header_t) + hdr->code_size;
        for (uint32_t i = 0; i < bytes; i++)
            dst[i] = src[i];
        total += bytes;
        src += bytes;
        dst += bytes;
    }

    /* Phase 2: now it's safe to zero BSS */
    for (uint8_t *p = _kernel_load_end; p < _kernel_end; p++)
        *p = 0;

    /* Phase 3: build name->entry table from the safely-copied data */
    prog_count = 0;
    hdr = (exec_header_t *)PROGRAMS_BASE;
    for (uint32_t off = 0; off < total; ) {
        if (hdr->magic != EXEC_MAGIC || prog_count >= MAX_PROGS)
            break;
        for (int i = 0; i < EXEC_NAME_MAX; i++)
            prog_table[prog_count].name[i] = hdr->name[i];
        prog_table[prog_count].entry = (void (*)(void))(uintptr_t)hdr->entry;
        prog_count++;
        off += sizeof(exec_header_t) + hdr->code_size;
        hdr = (exec_header_t *)((uint8_t *)PROGRAMS_BASE + off);
    }
}

int exec(const char *name) {
    for (int i = 0; i < prog_count; i++) {
        const char *a = name;
        const char *b = prog_table[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            prog_table[i].entry();
            return 0;
        }
    }
    return -1;
}
