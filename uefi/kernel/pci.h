#ifndef MINIOS_PCI_H
#define MINIOS_PCI_H

/* Full CF8/CFC scan, one line per function. */
void pci_dump(void);

/* Detailed BAR/capability dump for the Wi-Fi target (or a QEMU NIC). */
void pci_dump_target(void);

#endif /* MINIOS_PCI_H */
