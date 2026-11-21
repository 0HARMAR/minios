#ifndef MINIOS_FW_H
#define MINIOS_FW_H

#include "bootinfo.h"

/* Full TLV-header dump for the firmware blob carried in BootInfo. */
void fw_dump(const struct minios_boot_info *bi);

/* One-line summary (name, size, API, build) for the last screen. */
void fw_summary(const struct minios_boot_info *bi);

#endif /* MINIOS_FW_H */
