#include "pci.h"
#include "io.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_ENABLE_BIT 0x80000000

uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    uint32_t addr = PCI_ENABLE_BIT
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8)
                  | ((uint32_t)reg & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t value) {
    uint32_t addr = PCI_ENABLE_BIT
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8)
                  | ((uint32_t)reg & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, value);
}

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *out) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read_config(0, dev, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF)
            continue;

        uint16_t v = id & 0xFFFF;
        uint16_t d = (id >> 16) & 0xFFFF;
        if (v != vendor || d != device)
            continue;

        out->bus    = 0;
        out->dev    = dev;
        out->func   = 0;
        out->vendor = v;
        out->device = d;
        out->bar0   = pci_read_config(0, dev, 0, 0x10);
        out->bar1   = pci_read_config(0, dev, 0, 0x14);
        out->irq    = (uint8_t)(pci_read_config(0, dev, 0, 0x3C) & 0xFF);
        return 1;
    }
    return 0;
}