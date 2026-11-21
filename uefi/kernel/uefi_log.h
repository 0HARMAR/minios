#ifndef MINIOS_UEFI_LOG_H
#define MINIOS_UEFI_LOG_H

#include "bootinfo.h"

/*
 * Persist the kernel log as a non-volatile UEFI variable
 * (vendor GUID 4d304f53-0001-4d30-9a4b-4d304c4f4731, name MiniOSM0Log).
 * Best effort: failures are logged and never halt the probe.
 */
void uefi_log_save(const struct minios_boot_info *bi);

#endif /* MINIOS_UEFI_LOG_H */
