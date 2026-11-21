/*
 * memmap.c - M0 UEFI memory-map and ACPI RSDP summary.
 *
 * The boot loader copies the final UEFI memory map below 4 GiB and stores a
 * 36-byte RSDP copy in BootInfo.  The kernel only reads those hand-off
 * buffers; it does not allocate or reclaim memory yet.
 */
#include "memmap.h"
#include "log.h"
#include "string.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

struct efi_mem_desc {
    u32 type;
    u32 pad;
    u64 physical_start;
    u64 virtual_start;
    u64 page_count;
    u64 attributes;
};

#define EFI_CONVENTIONAL_MEMORY 7u
#define PAGE_BYTES 4096ULL

void memmap_dump(const struct minios_boot_info *bi)
{
    const u8 *base;
    u64 count;
    u64 i;
    u64 conventional_bytes = 0;
    u64 conventional_ranges = 0;
    u64 largest = 0;

    if (bi->memory_map == 0 || bi->memory_map_desc_size == 0) {
        klog("\nMemMap: not provided by the loader\n");
        return;
    }

    base = (const u8 *)(unsigned long)bi->memory_map;
    count = bi->memory_map_size / bi->memory_map_desc_size;

    klog("\nUEFI memory map: %llu descriptors at 0x%llx "
         "(desc %u bytes, version %u)\n",
         count, bi->memory_map, bi->memory_map_desc_size,
         bi->memory_map_desc_version);

    for (i = 0; i < count; i++) {
        const struct efi_mem_desc *d =
            (const struct efi_mem_desc *)(base + i * bi->memory_map_desc_size);
        u64 bytes = d->page_count * PAGE_BYTES;

        if (d->type != EFI_CONVENTIONAL_MEMORY)
            continue;

        conventional_bytes += bytes;
        conventional_ranges++;
        if (bytes > largest)
            largest = bytes;
        if (conventional_ranges <= 6)
            klog("  conv %02llu: 0x%010llx-0x%010llx %llu KiB\n",
                 conventional_ranges, d->physical_start,
                 d->physical_start + bytes, bytes >> 10);
    }

    klog("  conventional: %llu MiB in %llu ranges, largest %llu MiB\n",
         conventional_bytes >> 20, conventional_ranges, largest >> 20);
    if (conventional_ranges > 6)
        klog("  (only the first 6 ranges are shown)\n");

    if (bi->acpi_rsdp_size >= 36) {
        const u8 *r = bi->acpi_rsdp_copy;
        char signature[9];
        char oem_id[7];
        u32 rsdt;
        u64 xsdt;
        unsigned int revision = r[15];

        memcpy(signature, r, 8);
        signature[8] = '\0';
        memcpy(oem_id, r + 9, 6);
        oem_id[6] = '\0';
        memcpy(&rsdt, r + 16, 4);
        if (revision >= 2) {
            memcpy(&xsdt, r + 24, 8);
            klog("ACPI RSDP: phys 0x%llx sig '%s' rev %u oem '%s' "
                 "rsdt 0x%08x xsdt 0x%llx\n",
                 bi->acpi_rsdp, signature, revision, oem_id, rsdt, xsdt);
        } else {
            klog("ACPI RSDP: phys 0x%llx sig '%s' rev %u oem '%s' "
                 "rsdt 0x%08x (ACPI 1.0, no xsdt)\n",
                 bi->acpi_rsdp, signature, revision, oem_id, rsdt);
        }
    } else {
        klog("ACPI RSDP: not provided by the loader\n");
    }
}
