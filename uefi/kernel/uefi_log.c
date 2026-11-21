/*
 * uefi_log.c - persist the M0 kernel log as a UEFI non-volatile variable.
 *
 * After ExitBootServices the kernel still runs with identity-mapped physical
 * addresses and no IDT.  This is deliberately best-effort and guarded: it
 * refuses to call runtime services if their code/data live above 4 GiB or the
 * table signature is wrong, so a broken firmware cannot turn M0 into a reset
 * loop before the screen log is visible.
 */
#include "uefi_log.h"
#include "log.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned short char16;
typedef u64 efi_status;

typedef struct {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
} efi_table_header;

typedef struct {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
} efi_guid;

typedef efi_status (__attribute__((ms_abi)) *efi_set_variable_t)(
    char16 *name, efi_guid *guid, u32 attributes, u64 data_size, void *data);

typedef struct {
    efi_table_header hdr;
    void *get_time;
    void *set_time;
    void *get_wakeup_time;
    void *set_wakeup_time;
    void *set_virtual_address_map;
    void *convert_pointer;
    void *get_variable;
    void *get_next_variable_name;
    efi_set_variable_t set_variable;
} efi_runtime_services;

#define EFI_RUNTIME_SERVICES_SIGNATURE 0x56524553544E5552ULL
#define EFI_VARIABLE_ATTRS 0x7u /* NV | BOOTSERVICE_ACCESS | RUNTIME_ACCESS */
#define LOW_4GIB 0x100000000ULL
#define PAGE_BYTES 4096ULL

struct efi_mem_desc {
    u32 type;
    u32 pad;
    u64 physical_start;
    u64 virtual_start;
    u64 page_count;
    u64 attributes;
};

#define EFI_RUNTIME_SERVICES_CODE 5u
#define EFI_RUNTIME_SERVICES_DATA 6u

static int runtime_below_4gib(const struct minios_boot_info *bi)
{
    const u8 *base;
    u64 count;
    u64 i;

    if (bi->memory_map == 0 || bi->memory_map_desc_size == 0)
        return 0;

    base = (const u8 *)(unsigned long)bi->memory_map;
    count = bi->memory_map_size / bi->memory_map_desc_size;
    for (i = 0; i < count; i++) {
        const struct efi_mem_desc *d =
            (const struct efi_mem_desc *)(base + i * bi->memory_map_desc_size);

        if (d->type != EFI_RUNTIME_SERVICES_CODE &&
            d->type != EFI_RUNTIME_SERVICES_DATA)
            continue;
        if (d->physical_start >= LOW_4GIB)
            return 0;
        if (d->page_count > (LOW_4GIB - d->physical_start) / PAGE_BYTES)
            return 0;
    }
    return 1;
}

void uefi_log_save(const struct minios_boot_info *bi)
{
    static char16 var_name[] = {
        'M', 'i', 'n', 'i', 'O', 'S', 'M', '0', 'L', 'o', 'g', 0
    };
    static efi_guid guid = {
        0x4D304F53u, 0x0001u, 0x4D30u,
        { 0x9A, 0x4B, 0x4D, 0x30, 0x4C, 0x4F, 0x47, 0x31 }
    };
    efi_runtime_services *rt;
    const char *data;
    unsigned long len;
    efi_status status;

    if (bi->runtime_services == 0 || bi->runtime_services >= LOW_4GIB) {
        klog("UEFI log: no usable runtime services\n");
        return;
    }
    if (!runtime_below_4gib(bi)) {
        klog("UEFI log: runtime code/data above 4 GiB, skipped\n");
        return;
    }

    rt = (efi_runtime_services *)(unsigned long)bi->runtime_services;
    if (rt->hdr.signature != EFI_RUNTIME_SERVICES_SIGNATURE) {
        klog("UEFI log: bad runtime header 0x%llx\n",
             (unsigned long long)rt->hdr.signature);
        return;
    }
    if (rt->set_variable == (efi_set_variable_t)0) {
        klog("UEFI log: SetVariable is NULL\n");
        return;
    }

    data = klog_data(&len);
    if (len > 8000)
        len = 8000;

    status = rt->set_variable(var_name, &guid, EFI_VARIABLE_ATTRS,
                              (u64)len, (void *)data);
    klog("UEFI log: SetVariable status=0x%llx len=%u\n",
         (unsigned long long)status, (unsigned)len);
}
