/*
 * fw.c - read-only parser for the Intel iwlwifi TLV firmware container.
 *
 * M0 only validates the container and prints its API/TLV list.  It does not
 * download sections to the device.  The header layout is:
 *
 *   u32 zero; u32 magic ("IWL\n" = 0x0a4c5749); char human[64];
 *   u32 ver; u32 build; u64 ignore;
 *
 * followed by TLVs: u32 type; u32 length; u8 data[length].
 */
#include "fw.h"
#include "log.h"
#include "string.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define IWL_FW_MAGIC 0x0A4C5749u
#define IWL_TLV_ALIGN 4u

struct fw_header {
    u32 zero;
    u32 magic;
    char human[64];
    u32 ver;
    u32 build;
    u64 ignore;
} __attribute__((packed));

static const char *tlv_name(u32 type)
{
    switch (type) {
    case 19: return "SEC_RT";
    case 20: return "SEC_INIT";
    case 21: return "SEC_WOWLAN";
    case 24: return "SECURE_RT";
    case 25: return "SECURE_INIT";
    case 26: return "SECURE_WOWLAN";
    case 34: return "SEC_RT_USNIFFER";
    case 36: return "FW_VERSION";
    default: return "";
    }
}

static int fw_parse(const struct minios_boot_info *bi, u32 *api, u32 *build,
                    char *human, unsigned long human_cap)
{
    const u8 *base;
    const struct fw_header *h;
    u64 offset;
    u32 header_api;
    u32 header_build;
    u32 tlv_api = 0;
    u32 tlv_build = 0;
    int have_tlv = 0;
    unsigned long n;

    if (bi->fw_base == 0 || bi->fw_size == 0)
        return 0;

    base = (const u8 *)(unsigned long)bi->fw_base;
    if (bi->fw_size < sizeof(*h))
        return 0;

    h = (const struct fw_header *)base;
    if (h->magic != IWL_FW_MAGIC || h->zero != 0)
        return 0;

    header_api = h->ver;
    header_build = h->build;

    if (human != (char *)0 && human_cap != 0) {
        n = 0;
        while (n + 1 < human_cap && n < sizeof(h->human) &&
               h->human[n] != '\0') {
            human[n] = h->human[n];
            n++;
        }
        human[n] = '\0';
    }

    offset = sizeof(*h);
    while (offset + 8 <= bi->fw_size) {
        u32 type;
        u32 length;

        memcpy(&type, base + offset, 4);
        memcpy(&length, base + offset + 4, 4);

        if ((u64)length > bi->fw_size - offset - 8)
            break;

        if (type == 36 && length >= 12) {
            u32 ver;
            u32 build_value;

            memcpy(&ver, base + offset + 8, 4);
            memcpy(&build_value, base + offset + 12, 4);
            tlv_api = ver;
            tlv_build = build_value;
            have_tlv = 1;
        }

        offset += 8 + (((u64)length + IWL_TLV_ALIGN - 1) & ~(u64)(IWL_TLV_ALIGN - 1));
    }

    *api = have_tlv ? tlv_api : header_api;
    *build = have_tlv ? tlv_build : header_build;
    return 1;
}

void fw_dump(const struct minios_boot_info *bi)
{
    const u8 *base;
    const struct fw_header *h;
    char human[65];
    u64 offset;
    int shown = 0;

    if (bi->fw_base == 0 || bi->fw_size == 0) {
        klog("\nFW: not loaded (PCI probe only)\n");
        return;
    }

    base = (const u8 *)(unsigned long)bi->fw_base;
    if (bi->fw_size < sizeof(*h)) {
        klog("\nFW: too small (%llu bytes)\n", bi->fw_size);
        return;
    }

    h = (const struct fw_header *)base;
    human[0] = '\0';
    if (h->magic == IWL_FW_MAGIC) {
        unsigned long i = 0;

        while (i + 1 < sizeof(human) && i < sizeof(h->human) &&
               h->human[i] != '\0') {
            human[i] = h->human[i];
            i++;
        }
        human[i] = '\0';
    }

    klog("\nFW: %s, %llu bytes @0x%llx\n", bi->fw_name, bi->fw_size,
         bi->fw_base);
    klog("  header: zero=%u magic=%08x ver=%u build=%08x\n",
         h->zero, h->magic, h->ver, h->build);
    klog("  human : %s\n", human);

    if (h->magic != IWL_FW_MAGIC) {
        klog("  FW: bad magic (expected %08x)\n", IWL_FW_MAGIC);
        return;
    }

    offset = sizeof(*h);
    while (offset + 8 <= bi->fw_size) {
        u32 type;
        u32 length;

        memcpy(&type, base + offset, 4);
        memcpy(&length, base + offset + 4, 4);

        if ((u64)length > bi->fw_size - offset - 8) {
            klog("  TLV %d @0x%04llx: bad length %u (remaining %llu)\n",
                 shown, offset, length, bi->fw_size - offset - 8);
            break;
        }

        klog("  TLV %2d @0x%04llx type %3u len %6u %s\n", shown, offset,
             type, length, tlv_name(type));

        if (type == 36 && length >= 12) {
            u32 ver;
            u32 build;
            u32 extra;

            memcpy(&ver, base + offset + 8, 4);
            memcpy(&build, base + offset + 12, 4);
            memcpy(&extra, base + offset + 16, 4);
            klog("       FW_VERSION: ver=%u build=%08x extra=%u\n",
                 ver, build, extra);
        }

        offset += 8 + (((u64)length + IWL_TLV_ALIGN - 1) &
                       ~(u64)(IWL_TLV_ALIGN - 1));
        shown++;
        if (shown >= 32) {
            klog("  ... more TLVs omitted\n");
            break;
        }
    }
    klog("  TLVs shown: %d\n", shown);
}

void fw_summary(const struct minios_boot_info *bi)
{
    u32 api = 0;
    u32 build = 0;

    if (!fw_parse(bi, &api, &build, (char *)0, 0)) {
        klog("FW: not loaded\n");
        return;
    }
    klog("FW: %s size %llu API %u build %08x\n", bi->fw_name, bi->fw_size,
         api, build);
}
