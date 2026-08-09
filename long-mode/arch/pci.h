#ifndef PCI_H
#define PCI_H

#include <stdint.h>

struct pci_device {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor;
    uint16_t device;
    uint32_t bar0;
    uint32_t bar1;
    uint8_t  irq;
};

uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t value);
/* scan bus 0 for vendor/device; returns 1 if found, fills out */
int pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *out);

#endif
