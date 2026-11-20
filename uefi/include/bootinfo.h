/*
 * bootinfo.h - hand-off structure passed from BOOTX64.EFI to the 64-bit kernel.
 *
 * Shared by the UEFI loader and the kernel, so it must stay free of EFI and
 * libc headers.  Both sides are compiled for x86-64 with the same alignment.
 */
#ifndef MINIOS_BOOTINFO_H
#define MINIOS_BOOTINFO_H

#define MINIOS_BOOT_MAGIC   0x4D494E4955454649ULL /* "MINIUEFI" */
#define MINIOS_BOOT_VERSION 1u

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

    unsigned long long reserved1[2];
};

#endif /* MINIOS_BOOTINFO_H */
