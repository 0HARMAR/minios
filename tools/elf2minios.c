/* elf2minios — convert a fully-linked ELF64 executable to MiniOS a.out
 *
 * Usage: elf2minios <input.elf> <output.aout> <name>
 *
 * Reads ELF program headers to extract text and data LOAD segments,
 * then writes a MiniOS a.out executable with the given program name.
 * No relocation or symbol tables are emitted — this produces final
 * executables only, not linkable objects. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ── ELF64 types ─────────────────────────────────────────────────── */

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

#define EI_NIDENT 16

typedef struct {
    uint8_t     e_ident[EI_NIDENT];
    Elf64_Half  e_type;
    Elf64_Half  e_machine;
    Elf64_Word  e_version;
    Elf64_Addr  e_entry;
    Elf64_Off   e_phoff;
    Elf64_Off   e_shoff;
    Elf64_Word  e_flags;
    Elf64_Half  e_ehsize;
    Elf64_Half  e_phentsize;
    Elf64_Half  e_phnum;
    Elf64_Half  e_shentsize;
    Elf64_Half  e_shnum;
    Elf64_Half  e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4

/* ── MiniOS a.out types (must match aout.h) ─────────────────────── */

#define AOUT_MAGIC   0x4D4F5554
#define AOUT_NAME_MAX 16

typedef struct __attribute__((packed)) {
    uint32_t a_magic;
    uint32_t a_text;
    uint32_t a_data;
    uint32_t a_bss;
    uint32_t a_syms;
    uint32_t a_trsize;
    uint32_t a_drsize;
    uint32_t a_version;
    uint64_t a_entry;
    uint64_t a_text_addr;
    char     a_name[AOUT_NAME_MAX];
} minios_exec_t;

/* ── helpers ─────────────────────────────────────────────────────── */

static void die(const char *msg)
{
    fprintf(stderr, "elf2minios: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void diex(const char *msg)
{
    fprintf(stderr, "elf2minios: %s\n", msg);
    exit(1);
}

static size_t fread_or_die(void *buf, size_t sz, FILE *f)
{
    size_t n = fread(buf, 1, sz, f);
    if (n != sz)
        die("short read");
    return n;
}

static void fwrite_or_die(const void *buf, size_t sz, FILE *f)
{
    size_t n = fwrite(buf, 1, sz, f);
    if (n != sz)
        die("short write");
}

/* ── main ────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: elf2minios <input.elf> <output.aout> <name>\n");
        return 1;
    }

    const char *elf_path   = argv[1];
    const char *aout_path  = argv[2];
    const char *name       = argv[3];
    size_t      name_len   = strlen(name);

    if (name_len == 0 || name_len > AOUT_NAME_MAX)
        diex("name must be 1–16 characters");

    /* ── open input ────────────────────────────────────────────── */
    FILE *fin = fopen(elf_path, "rb");
    if (!fin) die("open input");

    /* ── read ELF header ───────────────────────────────────────── */
    Elf64_Ehdr ehdr;
    fread_or_die(&ehdr, sizeof(ehdr), fin);

    /* validate */
    if (memcmp(ehdr.e_ident, "\x7f""ELF", 4) != 0)
        diex("not an ELF file");
    if (ehdr.e_ident[4] != 2)  /* ELFCLASS64 */
        diex("not a 64-bit ELF");
    if (ehdr.e_ident[5] != 1)  /* ELFDATA2LSB */
        diex("not little-endian");
    if (ehdr.e_machine != 0x3E) /* EM_X86_64 */
        diex("not x86_64");
    if (ehdr.e_type != 2)       /* ET_EXEC */
        diex("not an executable (ET_EXEC)");

    /* ── read program headers ──────────────────────────────────── */
    if (ehdr.e_phoff == 0 || ehdr.e_phnum == 0)
        diex("no program headers");

    Elf64_Phdr *phdrs = calloc(ehdr.e_phnum, sizeof(Elf64_Phdr));
    if (!phdrs) die("out of memory");

    if (fseek(fin, ehdr.e_phoff, SEEK_SET) != 0)
        die("seek to program headers");
    fread_or_die(phdrs, ehdr.e_phnum * sizeof(Elf64_Phdr), fin);

    /* ── scan LOAD segments ────────────────────────────────────── */
    Elf64_Phdr *text_ph = NULL;  /* RE  segment (code + rodata) */
    Elf64_Phdr *data_ph = NULL;  /* RW  segment (data + bss)   */

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        int has_x = (phdrs[i].p_flags & PF_X) != 0;
        int has_w = (phdrs[i].p_flags & PF_W) != 0;

        if (has_x && !has_w)
            text_ph = &phdrs[i];
        else if (has_w)
            data_ph = &phdrs[i];
        else if (!text_ph)
            text_ph = &phdrs[i];  /* R segment → treat as text */
    }

    if (!text_ph)
        diex("no loadable text segment found");

    /* ── extract segment sizes ─────────────────────────────────── */
    uint64_t text_addr = text_ph->p_vaddr;
    uint64_t text_sz   = text_ph->p_filesz;
    uint64_t data_sz   = 0;
    uint64_t bss_sz    = 0;
    if (data_ph) {
        data_sz = data_ph->p_filesz;
        bss_sz  = data_ph->p_memsz - data_ph->p_filesz;
    }

    uint64_t entry = ehdr.e_entry;

    printf("elf2minios: %s -> %s  name=%s\n", elf_path, aout_path, name);
    printf("  text: addr=0x%lx  size=%lu\n", text_addr, text_sz);
    printf("  data: size=%lu  bss=%lu\n", data_sz, bss_sz);
    printf("  entry: 0x%lx\n", entry);

    /* ── build a.out header ────────────────────────────────────── */
    minios_exec_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.a_magic     = AOUT_MAGIC;
    hdr.a_text      = text_sz;
    hdr.a_data      = data_sz;
    hdr.a_bss       = bss_sz;
    hdr.a_syms      = 0;        /* no symbol table in final executable */
    hdr.a_trsize    = 0;        /* no relocations needed */
    hdr.a_drsize    = 0;
    hdr.a_version   = 0;
    hdr.a_entry     = entry;
    hdr.a_text_addr = text_addr;
    memset(hdr.a_name, 0, AOUT_NAME_MAX);
    memcpy(hdr.a_name, name, name_len);

    /* ── write a.out ───────────────────────────────────────────── */
    FILE *fout = fopen(aout_path, "wb");
    if (!fout) die("open output");

    /* 1. header */
    fwrite_or_die(&hdr, sizeof(hdr), fout);

    /* 2. text segment — copy from ELF */
    {
        uint8_t *text_buf = malloc(text_sz);
        if (!text_buf) die("out of memory");
        if (fseek(fin, text_ph->p_offset, SEEK_SET) != 0)
            die("seek to text segment");
        fread_or_die(text_buf, text_sz, fin);
        fwrite_or_die(text_buf, text_sz, fout);
        free(text_buf);
    }

    /* 3. data segment — copy from ELF (if any) */
    if (data_sz > 0 && data_ph) {
        uint8_t *data_buf = malloc(data_sz);
        if (!data_buf) die("out of memory");
        if (fseek(fin, data_ph->p_offset, SEEK_SET) != 0)
            die("seek to data segment");
        fread_or_die(data_buf, data_sz, fin);
        fwrite_or_die(data_buf, data_sz, fout);
        free(data_buf);
    }

    fclose(fout);
    fclose(fin);

    printf("  output: header=%zu  text=%lu  data=%lu  bss=%lu  total=%lu\n",
           sizeof(hdr), text_sz, data_sz, bss_sz,
           (uint64_t)(sizeof(hdr) + text_sz + data_sz));

    return 0;
}
