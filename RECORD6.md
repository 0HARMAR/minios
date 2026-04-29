# RECORD6 — Simple Flat Filesystem (SIMP)

Adds a simple flat filesystem to the 64-bit long-mode kernel. No directories; operations are create, write, read only. The filesystem lives on a dedicated region of the disk and is accessed via a new ATA PIO block driver.

## New Files

| File | Purpose |
|------|---------|
| `long-mode/kernel/lib.h` / `lib.c` | Kernel utility functions: `memcpy`, `memset`, `strcmp`, `strlen`, `utoa` |
| `long-mode/drivers/ata.h` / `ata.c` | ATA PIO block driver (LBA28, primary IDE bus 0x1F0) |
| `long-mode/kernel/fs.h` / `fs.c` | Filesystem: SuperBlock, Inode, bitmaps, mkfs/create/write/read/list |

## Modified Files

| File | Change |
|------|--------|
| `long-mode/io/io.h` / `io.c` | Added `inw`/`outw` (16-bit port I/O) for ATA data transfer |
| `long-mode/boot_loader.S` | Preserves BIOS-provided DL (drive number) instead of hardcoding `0x00`. Variables moved to data section (`boot_drive`). Works with both floppy (`-fda`) and HDD (`-hda`). |
| `long-mode/build_64_boot.sh` | Compiles new `.c` files; creates 10 MB raw disk image (20480 sectors) instead of 1.44 MB floppy |
| `long-mode/kernel/kernel.c` | Filesystem init at boot, six new shell commands: `mkfs`, `touch`, `write`, `cat`, `ls`, `help` |

## ATA PIO Driver

The driver uses polling (no IRQ) to read/write single 512-byte sectors via the primary IDE controller at ports `0x1F0`–`0x1F7`. LBA28 addressing supports up to 128 GB.

- `ata_read_sector(lba, buf)` — issues READ SECTORS (0x20), waits for DRQ, reads 256 words via `inw`
- `ata_write_sector(lba, buf)` — issues WRITE SECTORS (0x30), writes 256 words via `outw`, then FLUSH CACHE (0xE7)

## Filesystem Design

### Disk Layout

```
Sector      0: MBR / Bootloader
Sectors  1–65: Kernel + Programs payload (contiguous)
Sectors 66–2047: Reserved (zero-filled)
Sector 2048+:   Filesystem region
```

Within the filesystem region (block = 1 sector = 512 bytes):

```
Block  0: SuperBlock     (512 B)
Block  1: Inode Bitmap   (512 B — tracks 64 inodes)
Block  2: Block Bitmap   (512 B — tracks 4096 data blocks)
Blocks 3–10: Inode Table (8 blocks × 8 inodes/block = 64 inodes)
Blocks 11+:  Data Blocks (up to 4096 blocks ≈ 2 MB)
```

### Structures

```c
#define FS_MAGIC      0x53494D50   // "SIMP"
#define DIRECT_PTRS   8
#define FS_NAME_MAX   16
#define FS_INODE_MAX  64
#define FS_BLOCKS_MAX 4096

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t data_start;
} SuperBlock;

typedef struct __attribute__((packed)) {
    char     name[FS_NAME_MAX];    // file name (no directories, so names live in inodes)
    uint32_t size;
    uint32_t blocks[DIRECT_PTRS];  // 8 direct block pointers → max 4 KB per file
    uint8_t  used;
    uint8_t  _pad[11];             // pad to 64 B so no inode straddles a sector boundary
} Inode;
```

### Bitmaps

Two byte-array bitmaps (one block each, bit-per-resource):

- **Inode bitmap** — bit `i` set → inode `i` is allocated
- **Block bitmap** — bit `i` set → data block `i` is allocated

Block `i` in the bitmap corresponds to disk sector `FS_START + data_start + i`.

### Operations

| Operation | Behavior |
|-----------|----------|
| `fs_mkfs()` | Writes SuperBlock with magic, zeros both bitmaps and the full inode table. Sets `g_ready = 1`. |
| `fs_create(name)` | Allocates a free inode (via bitmap), writes a zeroed Inode with `used=1`. Returns inode index or `-1` (full / error) / `-2` (duplicate name). |
| `fs_write(ino, data, size)` | Frees old data blocks, allocates new blocks (up to 8), writes data to each block, updates Inode. Max file size: 8 × 512 = 4 KB. |
| `fs_read(ino, buf, size)` | Reads up to `size` bytes from the file's data blocks into `buf`. Returns bytes read. |
| `fs_find(name)` | Linear scan of the inode table, comparing names. Returns inode index or `-1`. |
| `fs_list(print)` | Iterates used inodes, formats `"name (size bytes)"` per line, calls the `print` callback. |

### Shell Commands

```
$ mkfs                          # format the filesystem
$ touch <name>                  # create an empty file
$ write <name> <content>        # write text content to file
$ cat <name>                    # print file content
$ ls                            # list all files with sizes
$ help                          # show available commands
```

Commands are parsed by a simple tokenizer (`next_word`) that null-terminates space-separated words in-place. Filesystem commands are matched before falling through to the existing program executor (`exec`), so programs and fs commands coexist in the same shell.

## Build & Run

```bash
cd long-mode && bash build_64_boot.sh
qemu-system-x86_64 -hda output/disk.img
```

The build creates a 10 MB raw disk image. The bootloader (sector 0) + payload (sectors 1+) occupy ~33 KB. The filesystem region starts at sector 2048 (1 MB offset) — the kernel prints `[fs ready]` if a valid SuperBlock magic is found, or `[fs not ready - type mkfs]` if not.
