#!/usr/bin/env python3
"""
mkdisk.py - build a GPT disk image with a FAT16 EFI System Partition.

The image is generated completely in Python (protective MBR, primary/backup
GPT, FAT16 BPB, FATs, root directory and sub-directories), so neither root
privileges nor mtools/sgdisk are required.

Layout (default 64 MiB):
    LBA 0          protective MBR
    LBA 1          primary GPT header
    LBA 2..33      primary partition entries
    LBA 2048       start of the ESP (1 MiB alignment)
    LBA total-34   end of the ESP
    LBA total-33   backup partition entries
    LBA total-1    backup GPT header

Usage:
    mkdisk.py OUTPUT.img BOOTX64.EFI KERNEL.BIN [--firmware WIFI9.UC]
"""

import argparse
import struct
import sys
import uuid
import zlib
from pathlib import Path

SECTOR = 512
PART_START_LBA = 2048          # 1 MiB
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
ESP_TYPE_GUID = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
DISK_GUID = uuid.uuid5(uuid.NAMESPACE_URL, "https://github.com/0HARMAR/minios/uefi-disk")
ESP_GUID = uuid.uuid5(uuid.NAMESPACE_URL, "https://github.com/0HARMAR/minios/uefi-esp")
FAT_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1   # 2026-01-01
VOLUME_LABEL = "MINIOS"
VOLUME_ID = 0x4D4F5321          # "MOS!"


def guid_le(value: uuid.UUID) -> bytes:
    """GPT stores the first three GUID fields little-endian, the rest as-is."""
    return (value.time_low.to_bytes(4, "little")
            + value.time_mid.to_bytes(2, "little")
            + value.time_hi_version.to_bytes(2, "little")
            + value.bytes[8:])


class FatNode:
    def __init__(self, name: str):
        self.name = name
        self.dirs = {}
        self.files = []          # list of (name, bytes)
        self.file_clusters = []  # list of (first_cluster, cluster_count)
        self.cluster = 0
        self.parent = None

    def child_dir(self, name: str) -> "FatNode":
        if name not in self.dirs:
            node = FatNode(name)
            node.parent = self
            self.dirs[name] = node
        return self.dirs[name]

    def add_file(self, name: str, data: bytes):
        self.files.append((name, data))


def add_path(root: FatNode, path: str, data: bytes):
    parts = path.replace("\\", "/").strip("/").split("/")
    node = root
    for part in parts[:-1]:
        node = node.child_dir(part)
    node.add_file(parts[-1], data)


def short_name(name: str) -> bytes:
    """Encode an 8.3 name into the 11-byte on-disk form."""
    if name in (".", ".."):
        return name.encode("ascii").ljust(11, b" ")
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    if len(base) > 8 or len(ext) > 3:
        raise ValueError(f"not an 8.3 name: {name}")
    return base.upper().encode("ascii").ljust(8, b" ") + ext.upper().encode("ascii").ljust(3, b" ")


def dir_entry(name: str, attr: int, cluster: int, size: int) -> bytes:
    e = bytearray(32)
    e[0:11] = short_name(name)
    e[11] = attr
    e[13] = 0
    struct.pack_into("<H", e, 14, 0)          # create time
    struct.pack_into("<H", e, 16, FAT_DATE)   # create date
    struct.pack_into("<H", e, 18, FAT_DATE)   # access date
    struct.pack_into("<H", e, 20, 0)          # cluster high
    struct.pack_into("<H", e, 22, 0)          # write time
    struct.pack_into("<H", e, 24, FAT_DATE)   # write date
    struct.pack_into("<H", e, 26, cluster & 0xFFFF)
    struct.pack_into("<I", e, 28, size & 0xFFFFFFFF)
    return bytes(e)


def fat16_geometry(part_sectors: int, spc: int, root_entries: int):
    reserved = 1
    num_fats = 2
    root_secs = (root_entries * 32 + SECTOR - 1) // SECTOR
    data = part_sectors - reserved - root_secs
    clusters = data // spc
    spf = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
    for _ in range(8):
        data = part_sectors - reserved - root_secs - num_fats * spf
        clusters = data // spc
        new_spf = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if new_spf == spf:
            break
        spf = new_spf
    return reserved, num_fats, root_secs, spf, clusters


def build_fat16(part_sectors: int, root: FatNode, spc: int = 8, root_entries: int = 512) -> bytes:
    reserved, num_fats, root_secs, spf, cluster_count = fat16_geometry(part_sectors, spc, root_entries)
    if not (4085 <= cluster_count <= 65524):
        raise ValueError(f"FAT16 cluster count out of range: {cluster_count}")

    cluster_bytes = spc * SECTOR
    fat_offset = reserved * SECTOR
    root_offset = (reserved + num_fats * spf) * SECTOR
    data_offset = (reserved + num_fats * spf + root_secs) * SECTOR
    image = bytearray(part_sectors * SECTOR)

    fat = [0] * (cluster_count + 2)
    fat[0] = 0xFFF8
    fat[1] = 0xFFFF
    next_free = 2

    def allocate(count: int) -> int:
        nonlocal next_free
        if count == 0:
            return 0
        if next_free + count > cluster_count + 2:
            raise ValueError("ESP is full")
        first = next_free
        next_free += count
        for c in range(first, first + count - 1):
            fat[c] = c + 1
        fat[first + count - 1] = 0xFFFF
        return first

    def alloc_tree(node: FatNode):
        if node is not root:
            node.cluster = allocate(1)
        for child in node.dirs.values():
            alloc_tree(child)
        node.file_clusters = []
        for _, data in node.files:
            n = (len(data) + cluster_bytes - 1) // cluster_bytes
            node.file_clusters.append((allocate(n), n))

    def put_cluster(cluster: int, payload: bytes):
        off = data_offset + (cluster - 2) * cluster_bytes
        image[off:off + len(payload)] = payload

    def write_dir(node: FatNode):
        entries = []
        if node is root:
            entries.append(dir_entry(VOLUME_LABEL, 0x08, 0, 0))
        else:
            entries.append(dir_entry(".", 0x10, node.cluster, 0))
            parent_cluster = 0 if node.parent is root else node.parent.cluster
            entries.append(dir_entry("..", 0x10, parent_cluster, 0))
        for child in node.dirs.values():
            entries.append(dir_entry(child.name, 0x10, child.cluster, 0))
        for (name, data), (first, _) in zip(node.files, node.file_clusters):
            entries.append(dir_entry(name, 0x20, first, len(data)))
        blob = b"".join(entries)
        if node is root:
            if len(blob) > root_entries * 32:
                raise ValueError("root directory overflow")
            image[root_offset:root_offset + len(blob)] = blob
        else:
            if len(blob) > cluster_bytes:
                raise ValueError(f"directory {node.name} does not fit in one cluster")
            put_cluster(node.cluster, blob)
        for child in node.dirs.values():
            write_dir(child)

    def write_files(node: FatNode):
        for (name, data), (first, _) in zip(node.files, node.file_clusters):
            put_cluster(first, data)
        for child in node.dirs.values():
            write_files(child)

    alloc_tree(root)
    write_dir(root)
    write_files(root)

    fat_bytes = bytearray(spf * SECTOR)
    for i, value in enumerate(fat):
        struct.pack_into("<H", fat_bytes, i * 2, value)
    for i in range(num_fats):
        off = fat_offset + i * spf * SECTOR
        image[off:off + len(fat_bytes)] = fat_bytes

    boot = bytearray(SECTOR)
    boot[0:3] = b"\xEB\x3C\x90"
    boot[3:11] = b"MINIOS  "
    struct.pack_into("<H", boot, 11, SECTOR)
    boot[13] = spc
    struct.pack_into("<H", boot, 14, reserved)
    boot[16] = num_fats
    struct.pack_into("<H", boot, 17, root_entries)
    struct.pack_into("<H", boot, 19, 0 if part_sectors >= 0x10000 else part_sectors)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, spf)
    struct.pack_into("<H", boot, 24, 63)
    struct.pack_into("<H", boot, 26, 255)
    struct.pack_into("<I", boot, 28, PART_START_LBA)
    struct.pack_into("<I", boot, 32, part_sectors)
    boot[36] = 0x80
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, VOLUME_ID)
    boot[43:54] = VOLUME_LABEL.ljust(11).encode("ascii")
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xAA"
    image[0:SECTOR] = boot

    return image


def build_gpt(total_lba: int, part_start: int, part_last: int):
    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    entry = bytearray(GPT_ENTRY_SIZE)
    entry[0:16] = guid_le(ESP_TYPE_GUID)
    entry[16:32] = guid_le(ESP_GUID)
    struct.pack_into("<Q", entry, 32, part_start)
    struct.pack_into("<Q", entry, 40, part_last)
    struct.pack_into("<Q", entry, 48, 0)
    name = "MINI-OS ESP".encode("utf-16-le")
    entry[56:56 + len(name)] = name
    entries[0:GPT_ENTRY_SIZE] = entry

    def header(current_lba: int, backup_lba: int, entries_lba: int) -> bytes:
        h = bytearray(SECTOR)
        h[0:8] = b"EFI PART"
        struct.pack_into("<I", h, 8, 0x00010000)
        struct.pack_into("<I", h, 12, 92)
        struct.pack_into("<I", h, 16, 0)          # CRC32, filled later
        struct.pack_into("<I", h, 20, 0)
        struct.pack_into("<Q", h, 24, current_lba)
        struct.pack_into("<Q", h, 32, backup_lba)
        struct.pack_into("<Q", h, 40, 34)
        struct.pack_into("<Q", h, 48, total_lba - 34)
        h[56:72] = guid_le(DISK_GUID)
        struct.pack_into("<Q", h, 72, entries_lba)
        struct.pack_into("<I", h, 80, GPT_ENTRY_COUNT)
        struct.pack_into("<I", h, 84, GPT_ENTRY_SIZE)
        struct.pack_into("<I", h, 88, zlib.crc32(entries) & 0xFFFFFFFF)
        struct.pack_into("<I", h, 16, zlib.crc32(h[:92]) & 0xFFFFFFFF)
        return bytes(h)

    primary = header(1, total_lba - 1, 2)
    backup = header(total_lba - 1, 1, total_lba - 33)
    return primary, backup, bytes(entries)


def protective_mbr(total_lba: int) -> bytes:
    mbr = bytearray(SECTOR)
    entry = bytearray(16)
    entry[1:4] = b"\x00\x02\x00"
    entry[4] = 0xEE
    entry[5:8] = b"\xFF\xFF\xFF"
    struct.pack_into("<I", entry, 8, 1)
    struct.pack_into("<I", entry, 12, min(total_lba - 1, 0xFFFFFFFF))
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"
    return bytes(mbr)


def main() -> int:
    ap = argparse.ArgumentParser(description="Build a GPT + FAT16 ESP image for Mini-OS UEFI")
    ap.add_argument("output")
    ap.add_argument("bootx64")
    ap.add_argument("kernel")
    ap.add_argument("--firmware", default=None,
                    help="optional firmware blob, installed as EFI/MINIOS/WIFI9.UC")
    ap.add_argument("--size-mib", type=int, default=64)
    args = ap.parse_args()

    boot_data = Path(args.bootx64).read_bytes()
    kernel_data = Path(args.kernel).read_bytes()
    firmware_data = b""
    if args.firmware:
        firmware_data = Path(args.firmware).read_bytes()
        if not firmware_data:
            raise SystemExit("error: firmware image is empty")
    if not boot_data:
        raise SystemExit("error: BOOTX64.EFI is empty")
    if not kernel_data:
        raise SystemExit("error: KERNEL.BIN is empty")

    total_bytes = args.size_mib * 1024 * 1024
    total_lba = total_bytes // SECTOR
    part_start = PART_START_LBA
    part_last = total_lba - 34
    part_sectors = part_last - part_start + 1
    if part_sectors <= 0:
        raise SystemExit("error: image is too small")

    root = FatNode("")
    add_path(root, "EFI/BOOT/BOOTX64.EFI", boot_data)
    add_path(root, "EFI/MINIOS/KERNEL.BIN", kernel_data)
    if firmware_data:
        add_path(root, "EFI/MINIOS/WIFI9.UC", firmware_data)

    fat = build_fat16(part_sectors, root)
    primary, backup, entries = build_gpt(total_lba, part_start, part_last)
    mbr = protective_mbr(total_lba)

    image = bytearray(total_bytes)
    image[0:SECTOR] = mbr
    image[SECTOR:2 * SECTOR] = primary
    image[2 * SECTOR:2 * SECTOR + len(entries)] = entries
    image[part_start * SECTOR:part_start * SECTOR + len(fat)] = fat
    image[(total_lba - 33) * SECTOR:(total_lba - 33) * SECTOR + len(entries)] = entries
    image[(total_lba - 1) * SECTOR:total_lba * SECTOR] = backup

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)

    print(f"image      : {out}")
    print(f"size       : {args.size_mib} MiB ({total_lba} sectors)")
    print(f"ESP        : LBA {part_start}..{part_last} ({part_sectors} sectors)")
    print(f"filesystem : FAT16, {8 * SECTOR} bytes/cluster")
    print(f"files      : EFI/BOOT/BOOTX64.EFI ({len(boot_data)} bytes)")
    print(f"             EFI/MINIOS/KERNEL.BIN ({len(kernel_data)} bytes)")
    if firmware_data:
        print(f"             EFI/MINIOS/WIFI9.UC ({len(firmware_data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
