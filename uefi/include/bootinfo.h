/*
 * bootinfo.h - hand-off structure passed from BOOTX64.EFI to the 64-bit kernel.
 *
 * Shared by the UEFI loader and the kernel, so it must stay free of EFI and
 * libc headers.  Both sides are compiled for x86-64 with the same alignment.
 */
#ifndef MINIOS_BOOTINFO_H
#define MINIOS_BOOTINFO_H

#define MINIOS_BOOT_MAGIC   0x4D494E4955454649ULL /* "MINIUEFI" */
#define MINIOS_BOOT_VERSION 3u                    /* v3: UEFI probe + runtime log */

struct minios_boot_info {
    unsigned long long magic;         /* MINIOS_BOOT_MAGIC                   */
    unsigned int       version;       /* MINIOS_BOOT_VERSION                 */

    /* linear framebuffer, filled from EFI_GRAPHICS_OUTPUT_PROTOCOL */
    unsigned int       fb_width;      /* pixels                              */
    unsigned int       fb_height;     /* pixels                              */
    unsigned int       fb_pitch;      /* pixels per scan line, not bytes     */
    unsigned int       fb_format;     /* EFI_GRAPHICS_PIXEL_FORMAT           */
    unsigned int       fb_bpp;        /* bits per pixel                      */
    unsigned int       mask_red;
    unsigned int       mask_green;
    unsigned int       mask_blue;
    unsigned int       mask_reserved;
    unsigned long long fb_base;       /* physical address                    */
    unsigned long long fb_size;       /* bytes                               */

    /* memory and image information */
    unsigned long long usable_ram;    /* conventional RAM, bytes             */
    unsigned long long kernel_base;   /* where the loader placed KERNEL.BIN  */
    unsigned long long kernel_size;   /* bytes                               */
    unsigned long long loader_base;   /* firmware load address of BOOTX64.EFI */
    unsigned long long loader_size;

    /* firmware identification, ASCII decoded from CHAR16 */
    unsigned int       firmware_revision;
    unsigned int       reserved0;
    char               firmware[64];

    /*
     * v2 probe hand-off.  All addresses are physical addresses that the
     * loader guarantees to be below 4 GiB, because the kernel's boot page
     * tables only identity-map the low 4 GiB (plus the framebuffer region).
     */
    unsigned long long memory_map;          /* EFI_MEMORY_DESCRIPTOR array */
    unsigned long long memory_map_size;     /* bytes */
    unsigned int       memory_map_desc_size;
    unsigned int       memory_map_desc_version;
    unsigned long long acpi_rsdp;           /* physical address, 0 if absent */
    unsigned int       acpi_rsdp_size;      /* copied bytes, <= 64 */
    unsigned int       reserved2;
    unsigned char      acpi_rsdp_copy[64];  /* safe copy for the kernel */
    unsigned long long fw_base;             /* firmware blob, < 4 GiB */
    unsigned long long fw_size;
    char               fw_name[16];         /* ESP 8.3 name, e.g. WIFI9.UC */
    unsigned long long reserved3[2];
    unsigned long long runtime_services;    /* EFI_RUNTIME_SERVICES * (v3) */
};

#endif /* MINIOS_BOOTINFO_H */
