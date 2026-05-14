#include "exec.h"

/* name -> entry_address table, built once at init */
static struct {
    char     name[AOUT_NAME_MAX];
    void   (*entry)(void);
    uint32_t offset;
} prog_table[MAX_PROGS];

extern void enter_user_mode(void *entry, void *stack);

static int prog_count;

/* _kernel_load_end: first byte after .data (where programs follow on disk).
 * _kernel_end:       first byte after .bss (BSS needs runtime zeroing). */
extern uint8_t _kernel_load_end[];
extern uint8_t _kernel_end[];

void exec_init(void) {
    uint8_t      *src = _kernel_load_end;
    uint8_t      *dst = (uint8_t *)PROGRAMS_BASE;
    minios_exec_t *hdr;
    uint32_t       moved = 0;

    /* Phase 1: copy programs out of the BSS-overlap region before
     * BSS zeroing destroys them.  The programs binary was loaded
     * right after the kernel on disk, which lands on _kernel_load_end
     * — the same memory the linker assigned to .bss. */
    while (1) {
        hdr = (minios_exec_t *)src;
        if (hdr->a_magic != AOUT_MAGIC)
            break;

        /* copy header + text + data (BSS has no file presence) */
        uint32_t file_bytes = A_HDRSIZE + hdr->a_text + hdr->a_data;
        for (uint32_t i = 0; i < file_bytes; i++)
            dst[i] = src[i];

        moved += file_bytes;
        src += file_bytes;
        dst += file_bytes;
    }

    /* Phase 2: now it's safe to zero the kernel's BSS */
    for (uint8_t *p = _kernel_load_end; p < _kernel_end; p++)
        *p = 0;

    /* Phase 3: zero programs' BSS and build name->entry table */
    prog_count = 0;
    uint8_t *prog = (uint8_t *)PROGRAMS_BASE;
    for (uint32_t off = 0; off < moved; ) {
        hdr = (minios_exec_t *)(prog + off);
        if (hdr->a_magic != AOUT_MAGIC || prog_count >= MAX_PROGS)
            break;

        /* zero-fill BSS after text+data */
        uint8_t *bss_start = prog + off + A_HDRSIZE + hdr->a_text + hdr->a_data;
        for (uint32_t i = 0; i < hdr->a_bss; i++)
            bss_start[i] = 0;

        /* record name -> entry */
        for (int i = 0; i < AOUT_NAME_MAX; i++)
            prog_table[prog_count].name[i] = hdr->a_name[i];
        prog_table[prog_count].entry = (void (*)(void))(uintptr_t)hdr->a_entry;
        prog_table[prog_count].offset = off;
        prog_count++;

        off += A_HDRSIZE + hdr->a_text + hdr->a_data;
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

int exec_user(const char *name) {
    for (int i = 0; i < prog_count; i++) {
        const char *a = name;
        const char *b = prog_table[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a != '\0' || *b != '\0')
            continue;

        minios_exec_t *hdr = (minios_exec_t *)((uint8_t *)PROGRAMS_BASE + prog_table[i].offset);
        uint8_t *src = (uint8_t *)hdr;

        uint64_t load_addr = hdr->a_text_addr;
        uint8_t *text = src + A_HDRSIZE;
        uint8_t *data = text + hdr->a_text;

        for (uint32_t j = 0; j < hdr->a_text; j++)
            ((uint8_t *)(uintptr_t)load_addr)[j] = text[j];

        for (uint32_t j = 0; j < hdr->a_data; j++)
            ((uint8_t *)(uintptr_t)(load_addr + hdr->a_text))[j] = data[j];

        for (uint32_t j = 0; j < hdr->a_bss; j++)
            ((uint8_t *)(uintptr_t)(load_addr + hdr->a_text + hdr->a_data))[j] = 0;

        enter_user_mode((void *)(uintptr_t)hdr->a_entry, (void *)0x400000);
        return 0;
    }
    return -1;
}
