/*
 * kernel.c - Mini-OS 64-bit kernel, UEFI boot milestone + M0 Wi-Fi probe.
 *
 * The kernel verifies BootInfo, prints long mode/CPU/memory/framebuffer
 * facts, then dumps the UEFI memory map, ACPI RSDP, PCI topology and the
 * optional iwlwifi firmware TLV header.  It does not program interrupts,
 * DMA or the Wi-Fi device yet.
 */
#include "bootinfo.h"
#include "format.h"
#include "serial.h"
#include "string.h"
#include "font8x16.h"
#include "log.h"
#include "memmap.h"
#include "pci.h"
#include "fw.h"
#include "uefi_log.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define FONT_W 8u
#define FONT_H 16u
#define FONT_GAP 2u
#define EFER_MSR 0xC0000080ULL

#define KLOG_BUF_SIZE 8192UL

static struct minios_boot_info *g_bi;
static char g_logbuf[KLOG_BUF_SIZE];
static unsigned long g_loglen;
static u8 *g_fb;
static u32 g_width;
static u32 g_height;
static u32 g_pitch;              /* pixels per scan line */
static u32 g_scale;
static u32 g_char_w;
static u32 g_line_h;
static u32 g_cx;
static u32 g_cy;
static u32 g_margin_x;
static u32 g_margin_y;
static u32 g_fg;
static u32 g_bg;

/* ------------------------------------------------------------------ */
/* CPU helpers                                                         */
/* ------------------------------------------------------------------ */
static void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

static u64 read_cr0(void)
{
    u64 v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static u64 read_cr4(void)
{
    u64 v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static u64 read_msr(u32 msr)
{
    u32 lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void cpu_brand(char *out, u32 cap)
{
    u32 eax, ebx, ecx, edx;
    u32 regs[12];
    u32 i;
    u32 n = 0;
    int started = 0;
    const char *p = (const char *)regs;

    cpuid(0x80000000u, 0, &eax, &ebx, &ecx, &edx);
    if (eax < 0x80000004u || cap < 2) {
        const char *unknown = "unknown x86-64 CPU";
        while (*unknown != '\0' && n + 1 < cap)
            out[n++] = *unknown++;
        out[n] = '\0';
        return;
    }

    for (i = 0; i < 3; i++)
        cpuid(0x80000002u + i, 0, &regs[i * 4], &regs[i * 4 + 1],
              &regs[i * 4 + 2], &regs[i * 4 + 3]);

    for (i = 0; i < 48 && n + 1 < cap; i++) {
        char c = p[i];
        if (!started) {
            if (c == ' ')
                continue;
            started = 1;
        }
        if (c == '\0')
            break;
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* framebuffer console                                                 */
/* ------------------------------------------------------------------ */
static u64 scale_mask(u32 mask, u32 value)
{
    u32 shift = 0;
    u32 maximum;

    if (mask == 0)
        return 0;
    while (((mask >> shift) & 1u) == 0)
        shift++;
    maximum = mask >> shift;
    return (((u64)value * maximum + 127) / 255) << shift;
}

static u32 rgb(u32 r, u32 g, u32 b)
{
    return (u32)scale_mask(g_bi->mask_red, r) |
           (u32)scale_mask(g_bi->mask_green, g) |
           (u32)scale_mask(g_bi->mask_blue, b);
}

static void fb_pixel(u32 x, u32 y, u32 color)
{
    if (x >= g_width || y >= g_height)
        return;
    *(volatile u32 *)(g_fb + (u64)y * g_pitch * 4 + (u64)x * 4) = color;
}

static void fb_clear(void)
{
    volatile u32 *p = (volatile u32 *)g_fb;
    u64 count = (u64)g_pitch * g_height;
    u64 i;

    for (i = 0; i < count; i++)
        p[i] = g_bg;
}

static void fb_scroll(void)
{
    u64 line_bytes = (u64)g_pitch * 4;
    u64 tail;
    volatile u32 *p;
    u64 i;

    if (g_line_h >= g_height)
        return;
    memmove(g_fb, g_fb + line_bytes * g_line_h,
            line_bytes * (g_height - g_line_h));
    tail = line_bytes * (g_height - g_line_h);
    p = (volatile u32 *)(g_fb + tail);
    for (i = 0; i < (u64)g_pitch * g_line_h; i++)
        p[i] = g_bg;
}

static void fb_newline(void)
{
    g_cx = g_margin_x;
    g_cy += g_line_h;
    if (g_cy + FONT_H * g_scale > g_height - g_margin_y) {
        fb_scroll();
        g_cy -= g_line_h;
    }
}

static void fb_glyph(char ch, u32 x, u32 y)
{
    u32 row, col, yy, xx;
    const unsigned char *glyph;

    if (ch < 0x20 || ch > 0x7e)
        ch = '?';
    glyph = font8x16[(u8)ch - 0x20];

    for (row = 0; row < FONT_H; row++) {
        u8 bits = glyph[row];
        if (bits == 0)
            continue;
        for (col = 0; col < FONT_W; col++) {
            if ((bits & (0x80u >> col)) == 0)
                continue;
            for (yy = 0; yy < g_scale; yy++) {
                for (xx = 0; xx < g_scale; xx++)
                    fb_pixel(x + col * g_scale + xx,
                             y + row * g_scale + yy, g_fg);
            }
        }
    }
}

static void fb_putc(char c)
{
    if (c == '\n') {
        fb_newline();
        return;
    }
    if (c == '\r') {
        g_cx = 0;
        return;
    }
    if (c == '\t') {
        u32 stop = g_char_w * 4;
        u32 target = (g_cx / stop + 1) * stop;
        while (g_cx < target)
            fb_putc(' ');
        return;
    }
    if (g_cx + g_char_w > g_width - g_margin_x)
        fb_newline();
    fb_glyph(c, g_cx, g_cy);
    g_cx += g_char_w;
}

static void fb_puts(const char *s)
{
    while (*s != '\0')
        fb_putc(*s++);
}

static int fb_init(void)
{
    u32 scale;

    if (g_bi->fb_base == 0 || g_bi->fb_width == 0 || g_bi->fb_height == 0 ||
        g_bi->fb_bpp != 32 || g_bi->mask_red == 0 ||
        g_bi->mask_green == 0 || g_bi->mask_blue == 0)
        return 0;
    if (g_bi->fb_pitch < g_bi->fb_width ||
        g_bi->fb_size < (u64)g_bi->fb_pitch * g_bi->fb_height * 4)
        return 0;

    g_fb = (u8 *)(unsigned long)g_bi->fb_base;
    g_width = g_bi->fb_width;
    g_height = g_bi->fb_height;
    g_pitch = g_bi->fb_pitch;

    scale = (g_height + 599u) / 600u;
    if (scale < 1)
        scale = 1;
    if (scale > 4)
        scale = 4;
    while (scale > 1 &&
           (g_width < FONT_W * scale || g_height < FONT_H * scale))
        scale--;
    g_scale = scale;
    g_char_w = FONT_W * g_scale;
    g_line_h = (FONT_H + FONT_GAP) * g_scale;

    g_fg = rgb(232, 232, 232);
    g_bg = rgb(8, 8, 8);
    g_margin_x = FONT_W * g_scale;
    g_margin_y = (FONT_H * g_scale) / 2;
    fb_clear();
    g_cx = g_margin_x;
    g_cy = g_margin_y;
    return 1;
}

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
void klog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    const char *p;

    va_start(ap, fmt);
    mini_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    for (p = buf; *p != '\0' && g_loglen + 1 < KLOG_BUF_SIZE; p++)
        g_logbuf[g_loglen++] = *p;
    g_logbuf[g_loglen] = '\0';

    serial_puts(buf);
    if (g_fb != (u8 *)0)
        fb_puts(buf);
}

const char *klog_data(unsigned long *len)
{
    if (len != (unsigned long *)0)
        *len = g_loglen;
    return g_logbuf;
}

static void halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */
void kernel_main(struct minios_boot_info *bi)
{
    char brand[64];
    char vendor[13];
    u64 efer;
    u64 cr0;
    u64 cr4;
    u32 eax, ebx, ecx, edx;

    serial_init();
    serial_puts("\n[kernel] entered 64-bit kernel\n");

    if (bi == (struct minios_boot_info *)0 ||
        bi->magic != MINIOS_BOOT_MAGIC ||
        bi->version != MINIOS_BOOT_VERSION) {
        serial_puts("[kernel] invalid BootInfo magic/version\n");
        halt_forever();
    }

    g_bi = bi;
    (void)fb_init();   /* if it fails, output is serial-only */

    efer = read_msr((u32)EFER_MSR);
    cr0 = read_cr0();
    cr4 = read_cr4();
    cpu_brand(brand, sizeof(brand));
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    memcpy(vendor + 0, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';

    klog("\nMini-OS 64-bit Kernel\n");
    klog("=====================\n\n");
    klog("BootInfo    : 0x%llx, magic OK, version %u\n",
            (unsigned long long)(unsigned long)bi, bi->version);
    klog("CPU         : %s\n", brand);
    klog("CPU vendor  : %s\n", vendor);
    klog("Long mode   : EFER.LME=%u, EFER.LMA=%u\n",
            (u32)((efer >> 8) & 1), (u32)((efer >> 10) & 1));
    klog("Paging      : CR0.PG=%u, CR4.PAE=%u, %u-level\n",
            (u32)((cr0 >> 31) & 1), (u32)((cr4 >> 5) & 1),
            (u32)(((cr4 >> 12) & 1) != 0 ? 5 : 4));

    if (bi->usable_ram != 0)
        klog("Usable RAM  : %llu MiB\n", bi->usable_ram >> 20);
    else
        klog("Usable RAM  : unknown\n");

    if (g_fb != (u8 *)0) {
        klog("Framebuffer : %ux%u, %u bpp, pitch %u, format %u\n",
                bi->fb_width, bi->fb_height, bi->fb_bpp, bi->fb_pitch,
                bi->fb_format);
        klog("FB address  : 0x%llx (%llu MiB)\n", bi->fb_base,
                bi->fb_size >> 20);
        klog("Text grid   : %u x %u cells, scale %u\n",
                (g_width - 2 * g_margin_x) / g_char_w,
                (g_height - 2 * g_margin_y) / g_line_h, g_scale);
    } else {
        klog("Framebuffer : unavailable, serial only\n");
    }

    klog("Kernel      : 0x%llx, %llu bytes\n",
            bi->kernel_base, bi->kernel_size);
    klog("Loader      : 0x%llx, %llu bytes\n",
            bi->loader_base, bi->loader_size);
    klog("Firmware    : %s (rev 0x%08x)\n",
            bi->firmware, bi->firmware_revision);
    klog("Boot path   : UEFI -> GOP -> ExitBootServices -> 64-bit kernel\n");
    klog("Console     : GOP framebuffer%s\n",
            serial_present() ? " + COM1" : " (COM1 not present)");

    klog("\n--- M0 Wi-Fi probe ---\n");
    memmap_dump(bi);
    pci_dump();
    fw_dump(bi);
    pci_dump_target();
    fw_summary(bi);

    klog("\n[ M0 DONE - SYSTEM HALTED ]\n");
    uefi_log_save(bi);
    halt_forever();
}
