#ifndef MINIOS_MEMMAP_H
#define MINIOS_MEMMAP_H

#include "bootinfo.h"

/* Summarize the UEFI memory map and the ACPI RSDP copy in BootInfo. */
void memmap_dump(const struct minios_boot_info *bi);

#endif /* MINIOS_MEMMAP_H */
