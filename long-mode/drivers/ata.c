/* ATA PIO driver — primary IDE bus, LBA28.
 * Uses polling I/O; no IRQ needed for this simple FS. */
#include "ata.h"
#include "io.h"

#define ATA_DATA    0x1F0
#define ATA_ERR     0x1F1
#define ATA_SECCNT  0x1F2
#define ATA_LBA_LO  0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI  0x1F5
#define ATA_DRV_SEL 0x1F6
#define ATA_CMD     0x1F7
#define ATA_STAT    0x1F7

#define ATA_CMD_READ  0x20
#define ATA_CMD_WRITE 0x30

#define SR_BSY  0x80
#define SR_DRDY 0x40
#define SR_DRQ  0x08
#define SR_ERR  0x01

static void ata_wait_bsy(void) {
    while (inb(ATA_STAT) & SR_BSY);
}

static void ata_wait_drq(void) {
    while ((inb(ATA_STAT) & SR_DRQ) == 0);
}

static void ata_select_drive(uint32_t lba) {
    outb(ATA_DRV_SEL, 0xE0 | ((lba >> 24) & 0x0F));  /* master, LBA */
    outb(ATA_SECCNT, 1);
    outb(ATA_LBA_LO,  lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI,  (lba >> 16) & 0xFF);
}

void ata_init(void) {
    /* brief reset: set SRST, then clear */
    outb(ATA_DRV_SEL, 0xE0);
    for (volatile int i = 0; i < 10000; i++);
    ata_wait_bsy();
}

int32_t ata_read_sector(uint32_t lba, uint8_t *buf) {
    ata_wait_bsy();
    ata_select_drive(lba);
    outb(ATA_CMD, ATA_CMD_READ);

    ata_wait_bsy();
    if (inb(ATA_STAT) & SR_ERR) return -1;
    ata_wait_drq();

    uint16_t *dst = (uint16_t *)buf;
    for (int i = 0; i < 256; i++)
        dst[i] = inw(ATA_DATA);

    return 0;
}

int32_t ata_write_sector(uint32_t lba, const uint8_t *buf) {
    ata_wait_bsy();
    ata_select_drive(lba);
    outb(ATA_CMD, ATA_CMD_WRITE);

    ata_wait_bsy();
    if (inb(ATA_STAT) & SR_ERR) return -1;
    ata_wait_drq();

    const uint16_t *src = (const uint16_t *)buf;
    for (int i = 0; i < 256; i++)
        outw(ATA_DATA, src[i]);

    /* flush write cache */
    outb(ATA_CMD, 0xE7);  /* FLUSH CACHE */
    ata_wait_bsy();

    return 0;
}
