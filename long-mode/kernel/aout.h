#ifndef AOUT_H
#define AOUT_H

#include <stdint.h>

/* ── MiniOS a.out executable/linkable format ─────────────────────── */

#define AOUT_MAGIC   0x4D4F5554   /* "MOUT" */
#define AOUT_VERSION 0
#define AOUT_NAME_MAX 16

/* ── Exec header (64 bytes packed) ───────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t a_magic;        /* AOUT_MAGIC */
    uint32_t a_text;         /* text  segment size in bytes */
    uint32_t a_data;         /* data  segment size in bytes */
    uint32_t a_bss;          /* BSS   segment size in bytes (file=0) */
    uint32_t a_syms;         /* symbol table size in bytes */
    uint32_t a_trsize;       /* text  relocation table size in bytes */
    uint32_t a_drsize;       /* data  relocation table size in bytes */
    uint32_t a_version;      /* format version, currently 0 */
    uint64_t a_entry;        /* entry point virtual address */
    uint64_t a_text_addr;    /* text segment load address */
    char     a_name[AOUT_NAME_MAX];  /* program name, null-padded */
} minios_exec_t;

/* ── Relocation entry (9 bytes packed) ───────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t r_offset;       /* byte offset within segment (text or data) */
    uint8_t  r_type;         /* relocation type: R_ABS or R_PC */
    uint32_t r_sym;          /* symbol table index */
} minios_reloc_t;

#define R_ABS  0             /* 64-bit absolute fixup  *(u64*)(P) += S     */
#define R_PC   1             /* 32-bit PC-relative      *(i32*)(P) += S-P  */

/* ── Symbol table entry (13 bytes packed) ────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t n_strx;         /* byte offset into string table */
    uint8_t  n_type;         /* N_UNDF | N_ABS | N_TEXT | N_DATA | N_BSS; + N_EXT */
    uint64_t n_value;        /* symbol value (address, or size for N_UNDF common) */
} minios_sym_t;

/* Symbol types — match original a.out values */
#define N_UNDF  0x00         /* undefined symbol */
#define N_EXT   0x01         /* external flag (OR with type) */
#define N_ABS   0x02         /* absolute symbol */
#define N_TEXT  0x04         /* text segment symbol */
#define N_DATA  0x06         /* data segment symbol */
#define N_BSS   0x08         /* BSS segment symbol */

/* ── Section offset macros ───────────────────────────────────────── */

#define A_HDRSIZE  ((uint32_t)sizeof(minios_exec_t))  /* 64 */

#define A_TXTOFF(x)   (A_HDRSIZE)
#define A_DATOFF(x)   (A_TXTOFF(x)  + (x).a_text)
#define A_TRELOFF(x)  (A_DATOFF(x)  + (x).a_data)
#define A_DRELOFF(x)  (A_TRELOFF(x) + (x).a_trsize)
#define A_SYMOFF(x)   (A_DRELOFF(x) + (x).a_drsize)
#define A_STROFF(x)   (A_SYMOFF(x)  + (x).a_syms)

/* count of entries in each table */
#define A_NSYM(x)   ((x).a_syms  / sizeof(minios_sym_t))
#define A_NTREL(x)  ((x).a_trsize / sizeof(minios_reloc_t))
#define A_NDREL(x)  ((x).a_drsize / sizeof(minios_reloc_t))

/* magic validation */
#define A_BADMAG(x)  ((x).a_magic != AOUT_MAGIC)

/* ── Load addresses ───────────────────────────────────────────────── */
#define A_TEXT_BASE(x)  ((x).a_text_addr)
#define A_DATA_BASE(x)  (A_TEXT_BASE(x) + (x).a_text)
#define A_BSS_BASE(x)   (A_DATA_BASE(x) + (x).a_data)

/* total memory footprint including BSS */
#define A_MEMSZ(x)      ((x).a_text + (x).a_data + (x).a_bss)

/* total file footprint (BSS occupies no file space) */
#define A_FILESZ(x)     (A_HDRSIZE + (x).a_text + (x).a_data + \
                         (x).a_trsize + (x).a_drsize + (x).a_syms)

#endif /* AOUT_H */
