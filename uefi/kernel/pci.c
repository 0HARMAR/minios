/*
 * pci.c - M0 PCI reconnaissance.
 *
 * This is deliberately read-mostly: it walks every bus/device/function over
 * CF8/CFC, prints the topology, and for the Wi-Fi target prints BARs and
 * capability registers.  BAR sizing writes all-ones and restores the original
 * value immediately; no device memory, bus-master enable or interrupt is
 * touched.
 */
#include "pci.h"
#include "io.h"
#include "log.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_ENABLE      0x80000000u

#define PCI_ID_INVALID  0xFFFFu

static u32 pci_read32(u8 bus, u8 dev, u8 func, u8 reg)
{
    u32 address = PCI_ENABLE | ((u32)bus << 16) | ((u32)dev << 11) |
                  ((u32)func << 8) | (reg & 0xFCu);

    outl(PCI_CONFIG_ADDR, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write32(u8 bus, u8 dev, u8 func, u8 reg, u32 value)
{
    u32 address = PCI_ENABLE | ((u32)bus << 16) | ((u32)dev << 11) |
                  ((u32)func << 8) | (reg & 0xFCu);

    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, value);
}

static u16 pci_read16(u8 bus, u8 dev, u8 func, u8 reg)
{
    u32 value = pci_read32(bus, dev, func, (u8)(reg & 0xFCu));

    return (u16)(value >> ((reg & 2u) * 8u));
}

static u8 pci_read8(u8 bus, u8 dev, u8 func, u8 reg)
{
    u32 value = pci_read32(bus, dev, func, (u8)(reg & 0xFCu));

    return (u8)(value >> ((reg & 3u) * 8u));
}

struct bar_info {
    int present;
    int is_io;
    int is_64;
    int prefetch;
    u64 addr;
    u64 size;
    u32 mask;
    u32 high_mask;
};

static struct bar_info pci_bar_info(u8 bus, u8 dev, u8 func, int bar)
{
    struct bar_info bi;
    u32 original;
    u32 mask;
    u32 high_original = 0;
    u32 high_mask = 0;

    bi.present = 0;
    bi.is_io = 0;
    bi.is_64 = 0;
    bi.prefetch = 0;
    bi.addr = 0;
    bi.size = 0;
    bi.mask = 0;
    bi.high_mask = 0;

    if (bar < 0 || bar > 5)
        return bi;

    original = pci_read32(bus, dev, func, (u8)(0x10 + bar * 4));
    if (original == 0 || original == 0xFFFFFFFFu)
        return bi;

    bi.present = 1;

    if ((original & 1u) != 0) {
        bi.is_io = 1;
        bi.addr = original & ~0x3u;
        pci_write32(bus, dev, func, (u8)(0x10 + bar * 4), 0xFFFFFFFFu);
        mask = pci_read32(bus, dev, func, (u8)(0x10 + bar * 4));
        pci_write32(bus, dev, func, (u8)(0x10 + bar * 4), original);
        bi.mask = mask;
        if (mask != 0 && mask != 0xFFFFFFFFu)
            bi.size = (u64)((~(mask & ~0x3u)) + 1u);
        return bi;
    }

    bi.is_64 = (original & 0x4u) != 0;
    bi.prefetch = (original & 0x8u) != 0;
    if (bi.is_64 && bar < 5)
        high_original = pci_read32(bus, dev, func, (u8)(0x14 + bar * 4));

    bi.addr = original & ~0xFu;
    if (bi.is_64 && bar < 5)
        bi.addr |= (u64)high_original << 32;

    pci_write32(bus, dev, func, (u8)(0x10 + bar * 4), 0xFFFFFFFFu);
    if (bi.is_64 && bar < 5)
        pci_write32(bus, dev, func, (u8)(0x14 + bar * 4), 0xFFFFFFFFu);

    mask = pci_read32(bus, dev, func, (u8)(0x10 + bar * 4));
    if (bi.is_64 && bar < 5)
        high_mask = pci_read32(bus, dev, func, (u8)(0x14 + bar * 4));

    pci_write32(bus, dev, func, (u8)(0x10 + bar * 4), original);
    if (bi.is_64 && bar < 5)
        pci_write32(bus, dev, func, (u8)(0x14 + bar * 4), high_original);

    bi.mask = mask;
    bi.high_mask = high_mask;
    if (mask != 0 && mask != 0xFFFFFFFFu) {
        if (bi.is_64 && bar < 5) {
            u64 all = (u64)(mask & ~0xFu) | ((u64)high_mask << 32);

            bi.size = (~all) + 1u;
        } else {
            bi.size = (u64)((~(mask & ~0xFu)) + 1u);
        }
    }
    return bi;
}

static int pci_is_9560(u16 vendor, u16 device)
{
    return vendor == 0x8086u &&
           (device == 0x34F0u || device == 0x9DF0u || device == 0xA370u ||
            device == 0x30DCu || device == 0x31DCu);
}

static int pci_is_network(u8 class, u8 subclass)
{
    /* 02/80 is Wi-Fi; 02/00 is Ethernet (QEMU NIC test fallback). */
    return class == 0x02u && (subclass == 0x80u || subclass == 0x00u);
}

#define PCI_MAX_DEVS 64

struct pci_dev_info {
    u8 bus;
    u8 dev;
    u8 func;
    u16 vendor;
    u16 device;
    u8 class;
    u8 subclass;
    u8 progif;
    u8 revision;
    u8 header;
};

static int pci_header_valid(u8 header)
{
    u8 base = header & 0x7Fu;

    return base == 0x00u || base == 0x01u || base == 0x02u;
}

/*
 * Modern Intel PCH returns stale/garbage data for absent functions instead
 * of 0xFFFF, so validate header type and class before trusting a function.
 */
static int pci_class_valid(u8 class, u8 subclass)
{
    if (class > 0x13u)
        return 0;
    if (class == 0x00u && subclass == 0x00u)
        return 0;
    return 1;
}

/*
 * M0 only needs bus 0: the target 9560 is 00:14.3.  Full ECAM/bridge
 * traversal is M1 work.  Function 1..7 are read only when function 0 says
 * the device is multi-function.
 */
static int pci_scan_bus0(struct pci_dev_info *out, int max)
{
    int count = 0;
    unsigned dev;
    unsigned func;

    if (max <= 0)
        return 0;

    for (dev = 0; dev < 32; dev++) {
        u32 id0 = pci_read32(0, (u8)dev, 0, 0);
        u16 vendor0 = (u16)(id0 & 0xFFFFu);
        u8 header0;
        unsigned functions;

        if (vendor0 == 0 || vendor0 == PCI_ID_INVALID)
            continue;

        header0 = (u8)(pci_read32(0, (u8)dev, 0, 0x0C) >> 16);
        if (!pci_header_valid(header0))
            continue;

        functions = (header0 & 0x80u) ? 8u : 1u;
        for (func = 0; func < functions; func++) {
            u32 id = pci_read32(0, (u8)dev, (u8)func, 0);
            u16 vendor = (u16)(id & 0xFFFFu);
            u16 device = (u16)(id >> 16);
            u32 classrev;
            u8 class;
            u8 subclass;
            u8 progif;
            u8 revision;
            u8 header;

            if (vendor == 0 || vendor == PCI_ID_INVALID)
                continue;

            classrev = pci_read32(0, (u8)dev, (u8)func, 0x08);
            class = (u8)(classrev >> 24);
            subclass = (u8)(classrev >> 16);
            progif = (u8)(classrev >> 8);
            revision = (u8)classrev;
            header = (u8)(pci_read32(0, (u8)dev, (u8)func, 0x0C) >> 16);

            if (!pci_header_valid(header))
                continue;
            if (!pci_class_valid(class, subclass))
                continue;

            if (count < max) {
                out[count].bus = 0;
                out[count].dev = (u8)dev;
                out[count].func = (u8)func;
                out[count].vendor = vendor;
                out[count].device = device;
                out[count].class = class;
                out[count].subclass = subclass;
                out[count].progif = progif;
                out[count].revision = revision;
                out[count].header = header;
            }
            count++;
            if (count >= max)
                return count;
        }
    }
    return count;
}

static void pci_dump_bars(u8 bus, u8 dev, u8 func)
{
    int bar;

    for (bar = 0; bar < 6; ) {
        struct bar_info bi = pci_bar_info(bus, dev, func, bar);

        if (!bi.present) {
            bar++;
            continue;
        }
        if (bi.is_io) {
            klog("  BAR%d I/O  addr 0x%llx size %llu\n", bar, bi.addr,
                 bi.size);
            bar++;
        } else {
            klog("  BAR%d %s%s addr 0x%llx size %llu mask=%08x%08x\n",
                 bar, bi.is_64 ? "64-bit " : "32-bit ",
                 bi.prefetch ? "prefetch" : "non-pref", bi.addr, bi.size,
                 bi.high_mask, bi.mask);
            bar += bi.is_64 ? 2 : 1;
        }
    }
}

static void pci_dump_caps(u8 bus, u8 dev, u8 func, u16 status)
{
    u8 cap;
    int guard = 0;

    if ((status & 0x10u) == 0) {
        klog("  caps: no capability list\n");
        return;
    }

    cap = (u8)(pci_read8(bus, dev, func, 0x34) & 0xFCu);
    while (cap >= 0x40u && guard < 48) {
        u8 id = pci_read8(bus, dev, func, cap);
        u8 next = (u8)(pci_read8(bus, dev, func, (u8)(cap + 1)) & 0xFCu);

        switch (id) {
        case 0x01: { /* power management */
            u16 pmc = pci_read16(bus, dev, func, (u8)(cap + 2));

            klog("  CAP 01 PM   @%02x version %u state %u\n", cap,
                 (unsigned)(pmc & 3u), (unsigned)((pmc >> 3) & 3u));
            break;
        }
        case 0x05: { /* MSI */
            u16 ctrl = pci_read16(bus, dev, func, (u8)(cap + 2));
            int is64 = (ctrl & 0x80u) != 0;
            u32 low = pci_read32(bus, dev, func, (u8)(cap + 4));
            u32 high = is64 ? pci_read32(bus, dev, func, (u8)(cap + 8)) : 0;
            u8 data_off = (u8)(is64 ? cap + 0x0C : cap + 8);
            u32 data = pci_read32(bus, dev, func, data_off);

            klog("  CAP 05 MSI  @%02x en=%u mmc=%u mmce=%u 64=%u "
                 "addr=%08x%08x data=%08x\n",
                 cap, (unsigned)(ctrl & 1u), (unsigned)((ctrl >> 1) & 7u),
                 (unsigned)((ctrl >> 4) & 7u), (unsigned)is64,
                 high, low, data);
            break;
        }
        case 0x10: { /* PCI Express */
            u16 flags = pci_read16(bus, dev, func, (u8)(cap + 2));
            u16 link = 0;

            if (cap + 0x14 <= 0x100)
                link = pci_read16(bus, dev, func, (u8)(cap + 0x12));
            klog("  CAP 10 PCIe @%02x ver=%u type=%u slot=%u "
                 "link_speed=%u link_width=%u\n",
                 cap, (unsigned)(flags & 0xFu),
                 (unsigned)((flags >> 4) & 0xFu),
                 (unsigned)((flags >> 8) & 1u),
                 (unsigned)(link & 0xFu), (unsigned)((link >> 4) & 0x3Fu));
            break;
        }
        case 0x11: { /* MSI-X */
            u16 ctrl = pci_read16(bus, dev, func, (u8)(cap + 2));
            u32 table = pci_read32(bus, dev, func, (u8)(cap + 4));
            u32 pba = pci_read32(bus, dev, func, (u8)(cap + 8));

            klog("  CAP 11 MSI-X@%02x en=%u mask=%u n=%u "
                 "table(bir=%u off=0x%x) pba(bir=%u off=0x%x)\n",
                 cap, (unsigned)((ctrl >> 15) & 1u),
                 (unsigned)((ctrl >> 14) & 1u),
                 (unsigned)((ctrl & 0x7FFu) + 1u),
                 (unsigned)(table & 7u), (unsigned)(table & ~7u),
                 (unsigned)(pba & 7u), (unsigned)(pba & ~7u));
            break;
        }
        default:
            klog("  CAP %02x     @%02x\n", id, cap);
            break;
        }

        if (next == 0 || next == cap)
            break;
        cap = next;
        guard++;
    }
    if (guard >= 48)
        klog("  caps: list truncated (loop?)\n");
}

static void pci_dump_one(u8 bus, u8 dev, u8 func, u16 vendor, u16 device,
                         u8 class, u8 subclass, u8 revision)
{
    u16 subsystem_vendor = pci_read16(bus, dev, func, 0x2C);
    u16 subsystem_id = pci_read16(bus, dev, func, 0x2E);
    u16 command = pci_read16(bus, dev, func, 0x04);
    u16 status = pci_read16(bus, dev, func, 0x06);
    u8 irq = pci_read8(bus, dev, func, 0x3C);
    u8 pin = pci_read8(bus, dev, func, 0x3D);

    klog("\n=== M0 target %02x:%02x.%x ===\n", bus, dev, func);
    klog("  ID %04x:%04x sub %04x:%04x rev %02x class %02x%02x\n",
         vendor, device, subsystem_vendor, subsystem_id, revision, class,
         subclass);
    klog("  CMD %04x STS %04x IRQ %u pin %u\n", command, status,
         (unsigned)irq, (unsigned)pin);
    pci_dump_bars(bus, dev, func);
    pci_dump_caps(bus, dev, func, status);
}

void pci_dump(void)
{
    struct pci_dev_info list[PCI_MAX_DEVS];
    int n;
    int i;

    n = pci_scan_bus0(list, PCI_MAX_DEVS);
    klog("\nPCI topology (bus 0, CF8/CFC; present functions only):\n");
    for (i = 0; i < n; i++) {
        const struct pci_dev_info *d = &list[i];

        klog("  %02x:%02x.%x %04x:%04x class %02x%02x.%02x "
             "rev %02x hdr %02x\n",
             (unsigned)d->bus, (unsigned)d->dev, (unsigned)d->func,
             (unsigned)d->vendor, (unsigned)d->device, (unsigned)d->class,
             (unsigned)d->subclass, (unsigned)d->progif,
             (unsigned)d->revision, (unsigned)d->header);
    }
    klog("PCI devices: %d%s\n", n,
         n >= PCI_MAX_DEVS ? " (truncated)" : "");
}

void pci_dump_target(void)
{
    struct pci_dev_info list[PCI_MAX_DEVS];
    int n;
    int i;
    int found = 0;

    n = pci_scan_bus0(list, PCI_MAX_DEVS);

    /* Prefer the real 9560 so a laptop's Ethernet cannot hide it on screen. */
    for (i = 0; i < n; i++) {
        const struct pci_dev_info *d = &list[i];

        if (!pci_is_9560(d->vendor, d->device))
            continue;
        pci_dump_one(d->bus, d->dev, d->func, d->vendor, d->device, d->class,
                     d->subclass, d->revision);
        found++;
    }
    if (found != 0) {
        klog("\nM0 target: %d 9560 device(s) dumped.\n", found);
        return;
    }

    for (i = 0; i < n; i++) {
        const struct pci_dev_info *d = &list[i];

        if (!pci_is_network(d->class, d->subclass))
            continue;
        pci_dump_one(d->bus, d->dev, d->func, d->vendor, d->device, d->class,
                     d->subclass, d->revision);
        klog("\nM0 target: no 9560; dumped the first network controller "
             "(QEMU/test NIC) on bus 0.\n");
        return;
    }

    klog("\nM0 target: no Wi-Fi/network controller on bus 0.\n");
}
