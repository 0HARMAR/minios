#ifndef ATA_H
#define ATA_H

#include <stdint.h>

void ata_init(void);
int32_t ata_read_sector(uint32_t lba, uint8_t *buf);
int32_t ata_write_sector(uint32_t lba, const uint8_t *buf);

#endif
