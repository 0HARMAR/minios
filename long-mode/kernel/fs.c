/* Simple flat filesystem — no directories, create/write/read only. */
#include "fs.h"
#include "ata.h"
#include "lib.h"

/* ── derived layout constants ─────────────────────────────── */
#define INODES_PER_BLOCK  (FS_BLOCK_SIZE / sizeof(Inode))   /* 8 */
#define INODE_TABLE_BLOCKS \
    ((FS_INODE_MAX + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK)  /* 8 */

/* block offsets relative to FS_START */
#define SB_BLOCK       0
#define INODE_BMAP     1
#define BLOCK_BMAP     2
#define INODE_TABLE    3
/* DATA_BLOCKS = INODE_TABLE + INODE_TABLE_BLOCKS */

/* ── static state ─────────────────────────────────────────── */
static SuperBlock g_sb;
static int32_t    g_ready = 0;
static uint8_t    g_buf[FS_BLOCK_SIZE];   /* general-purpose block buffer */

/* ── helpers ──────────────────────────────────────────────── */

static inline uint32_t data_start(void) { return g_sb.data_start; }

/* read/write a block (block = offset from FS_START) */
static int32_t rd_blk(uint32_t blk, uint8_t *buf) {
    return ata_read_sector(FS_START + blk, buf);
}
static int32_t wr_blk(uint32_t blk, const uint8_t *buf) {
    return ata_write_sector(FS_START + blk, buf);
}

/* ── bitmap ops (byte-array bitmaps) ──────────────────────── */

static void bm_set(uint8_t *bm, uint32_t bit) {
    bm[bit / 8] |= (1 << (bit % 8));
}
static void bm_clear(uint8_t *bm, uint32_t bit) {
    bm[bit / 8] &= ~(1 << (bit % 8));
}
static int32_t bm_find_free(uint8_t *bm, uint32_t max) {
    for (uint32_t i = 0; i < max; i++)
        if (!(bm[i / 8] & (1 << (i % 8)))) return i;
    return -1;
}

static int32_t bm_alloc(uint8_t *bm, uint32_t max) {
    int32_t bit = bm_find_free(bm, max);
    if (bit >= 0) bm_set(bm, bit);
    return bit;
}

/* ── inode helpers ────────────────────────────────────────── */

/* read inode i into *ino.  returns 0 on success. */
static int32_t inode_read(uint32_t idx, Inode *ino) {
    uint32_t blk  = INODE_TABLE + idx / INODES_PER_BLOCK;
    uint32_t off  = (idx % INODES_PER_BLOCK) * sizeof(Inode);
    if (rd_blk(blk, g_buf)) return -1;
    memcpy(ino, g_buf + off, sizeof(Inode));
    return 0;
}

/* write inode i back to disk */
static int32_t inode_write(uint32_t idx, const Inode *ino) {
    uint32_t blk  = INODE_TABLE + idx / INODES_PER_BLOCK;
    uint32_t off  = (idx % INODES_PER_BLOCK) * sizeof(Inode);
    if (rd_blk(blk, g_buf)) return -1;        /* read-modify-write */
    memcpy(g_buf + off, ino, sizeof(Inode));
    return wr_blk(blk, g_buf);
}

/* ── public API ───────────────────────────────────────────── */

void fs_init(void) {
    g_ready = 0;
    ata_init();
    if (rd_blk(SB_BLOCK, (uint8_t *)&g_sb)) return;
    if (g_sb.magic == FS_MAGIC) g_ready = 1;
}

int32_t fs_is_ready(void) { return g_ready; }

void fs_mkfs(void) {
    /* figure sizes */
    uint32_t data_blk = INODE_TABLE + INODE_TABLE_BLOCKS;  /* = 3 + 8 = 11 */
    uint32_t total    = data_blk + FS_BLOCKS_MAX;          /* = 11 + 4096 = 4107 */

    g_sb.magic       = FS_MAGIC;
    g_sb.total_blocks = total;
    g_sb.inode_count  = FS_INODE_MAX;
    g_sb.data_start   = data_blk;
    wr_blk(SB_BLOCK, (const uint8_t *)&g_sb);

    /* zero inode bitmap */
    memset(g_buf, 0, FS_BLOCK_SIZE);
    wr_blk(INODE_BMAP, g_buf);

    /* zero block bitmap */
    wr_blk(BLOCK_BMAP, g_buf);

    /* zero inode table */
    for (uint32_t i = 0; i < INODE_TABLE_BLOCKS; i++) {
        wr_blk(INODE_TABLE + i, g_buf);
    }

    g_ready = 1;
}

int32_t fs_create(const char *name) {
    if (!g_ready) return -1;

    /* check duplicate */
    if (fs_find(name) >= 0) return -2;

    /* read inode bitmap */
    if (rd_blk(INODE_BMAP, g_buf)) return -1;
    int32_t idx = bm_alloc(g_buf, g_sb.inode_count);
    if (idx < 0) return -1;
    wr_blk(INODE_BMAP, g_buf);

    /* write inode */
    Inode ino;
    memset(&ino, 0, sizeof(ino));
    uint32_t n = strlen(name);
    for (uint32_t i = 0; i < n && i < FS_NAME_MAX - 1; i++)
        ino.name[i] = name[i];
    ino.name[(n < FS_NAME_MAX) ? n : FS_NAME_MAX - 1] = '\0';
    ino.used = 1;
    inode_write(idx, &ino);

    return idx;
}

int32_t fs_write(int32_t ino_idx, const uint8_t *data, uint32_t size) {
    if (!g_ready || ino_idx < 0 || (uint32_t)ino_idx >= g_sb.inode_count)
        return -1;

    Inode ino;
    if (inode_read(ino_idx, &ino)) return -1;
    if (!ino.used) return -1;

    /* free old blocks */
    if (rd_blk(BLOCK_BMAP, g_buf)) return -1;
    for (int i = 0; i < DIRECT_PTRS && ino.blocks[i] != 0; i++) {
        bm_clear(g_buf, ino.blocks[i]);
        ino.blocks[i] = 0;
    }
    wr_blk(BLOCK_BMAP, g_buf);

    /* allocate new blocks */
    uint32_t needed = (size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
    if (needed > DIRECT_PTRS) return -1;
    if (needed == 0) { ino.size = 0; inode_write(ino_idx, &ino); return 0; }

    if (rd_blk(BLOCK_BMAP, g_buf)) return -1;
    uint32_t alloced[DIRECT_PTRS];
    int32_t ok = 1;
    for (uint32_t i = 0; i < needed; i++) {
        int32_t blk = bm_alloc(g_buf, FS_BLOCKS_MAX);
        if (blk < 0) { ok = 0; break; }
        alloced[i] = (uint32_t)blk;
        ino.blocks[i] = (uint32_t)blk;
    }
    if (!ok) {
        /* rollback */
        for (uint32_t i = 0; i < needed && ino.blocks[i]; i++)
            bm_clear(g_buf, ino.blocks[i]);
        wr_blk(BLOCK_BMAP, g_buf);
        return -1;
    }
    wr_blk(BLOCK_BMAP, g_buf);

    /* write data */
    for (uint32_t i = 0; i < needed; i++) {
        uint32_t chunk = (size - i * FS_BLOCK_SIZE > FS_BLOCK_SIZE)
                             ? FS_BLOCK_SIZE
                             : size - i * FS_BLOCK_SIZE;
        memcpy(g_buf, data + i * FS_BLOCK_SIZE, chunk);
        if (chunk < FS_BLOCK_SIZE)
            memset(g_buf + chunk, 0, FS_BLOCK_SIZE - chunk);
        wr_blk(data_start() + alloced[i], g_buf);
    }

    ino.size = size;
    inode_write(ino_idx, &ino);
    return (int32_t)size;
}

int32_t fs_read(int32_t ino_idx, uint8_t *buf, uint32_t size) {
    if (!g_ready || ino_idx < 0 || (uint32_t)ino_idx >= g_sb.inode_count)
        return -1;

    Inode ino;
    if (inode_read(ino_idx, &ino)) return -1;
    if (!ino.used) return -1;

    uint32_t total = ino.size;
    if (size > total) size = total;
    if (size == 0) return 0;

    uint32_t nblocks = (total + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
    uint32_t remain = size;
    for (uint32_t i = 0; i < nblocks && remain > 0; i++) {
        uint32_t chunk = (remain > FS_BLOCK_SIZE) ? FS_BLOCK_SIZE : remain;
        rd_blk(data_start() + ino.blocks[i], g_buf);
        memcpy(buf + i * FS_BLOCK_SIZE, g_buf, chunk);
        remain -= chunk;
    }
    return (int32_t)size;
}

int32_t fs_find(const char *name) {
    if (!g_ready) return -1;
    Inode ino;
    for (uint32_t i = 0; i < g_sb.inode_count; i++) {
        if (inode_read(i, &ino)) continue;
        if (ino.used && strcmp(ino.name, name) == 0) return (int32_t)i;
    }
    return -1;
}

void fs_list(void (*print)(const char *)) {
    if (!g_ready) return;
    Inode ino;
    char line[64];
    for (uint32_t i = 0; i < g_sb.inode_count; i++) {
        if (inode_read(i, &ino)) continue;
        if (!ino.used) continue;
        memset(line, 0, sizeof(line));
        memcpy(line, ino.name, FS_NAME_MAX);
        uint32_t nl = strlen(line);
        line[nl++] = ' ';
        line[nl++] = '(';
        utoa(ino.size, line + nl);
        while (line[nl]) nl++;
        line[nl++] = ' ';
        if (ino.size == 1) {
            memcpy(line + nl, "byte)", 5);
            nl += 5;
        } else {
            memcpy(line + nl, "bytes)", 6);
            nl += 6;
        }
        line[nl] = '\0';
        print(line);
    }
}
