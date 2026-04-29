#ifndef FS_H
#define FS_H

#include <stdint.h>

/* ── disk layout ──────────────────────────────────────────── */
#define FS_START      2048       /* first sector of filesystem */
#define FS_BLOCK_SIZE 512        /* 1 block = 1 sector */
#define FS_MAGIC      0x53494D50 /* "SIMP" */

/* ── sizing ───────────────────────────────────────────────── */
#define DIRECT_PTRS   8
#define FS_NAME_MAX   16
#define FS_INODE_MAX  64         /* max files */
#define FS_BLOCKS_MAX 4096       /* max data blocks (bitmap fits in 1 sector) */

/* ── structures ───────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t data_start;
} SuperBlock;

typedef struct __attribute__((packed)) {
    char     name[FS_NAME_MAX];
    uint32_t size;
    uint32_t blocks[DIRECT_PTRS];
    uint8_t  used;
    uint8_t  _pad[11];           /* 64 bytes total — no sector straddling */
} Inode;

/* ── API ──────────────────────────────────────────────────── */
void    fs_init(void);
int32_t fs_is_ready(void);
void    fs_mkfs(void);

int32_t fs_create(const char *name);
int32_t fs_write(int32_t ino, const uint8_t *data, uint32_t size);
int32_t fs_read(int32_t ino, uint8_t *buf, uint32_t size);
int32_t fs_find(const char *name);
void    fs_list(void (*print)(const char *));

#endif
