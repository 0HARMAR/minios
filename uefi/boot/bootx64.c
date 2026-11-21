/*
 * bootx64.c - the Mini-OS UEFI loader.
 *
 * Responsibilities:
 *   1. take the framebuffer description from GOP (do not change the mode),
 *   2. load \EFI\MINIOS\KERNEL.BIN from the ESP into physical 0x100000,
 *   3. build identity page tables for low memory (plus the loader and the
 *      framebuffer if they live above 4 GiB),
 *   4. leave boot services and jump to the flat-binary kernel in 64-bit mode.
 *
 * For the M0 Wi-Fi probe the loader also:
 *   - copies the final UEFI memory map to memory below 4 GiB,
 *   - copies the ACPI RSDP into BootInfo,
 *   - loads \EFI\MINIOS\WIFI9.UC (optional, 8.3 name) below 4 GiB.
 *
 * The kernel receives a pointer to struct minios_boot_info in RDI.
 */
#include <stddef.h>

#include "efi.h"
#include "bootinfo.h"
#include "format.h"
#include "serial.h"
#include "string.h"

#define KERNEL_LOAD_ADDR   0x100000ULL
#define KERNEL_MAX_SIZE    (16ULL * 1024 * 1024)
#define FW_MAX_SIZE        (8ULL * 1024 * 1024)
#define PAGE_SIZE          4096ULL
#define LOW_4GIB          0x100000000ULL
#define REGION_1GIB       0x40000000ULL
#define RSDP_COPY_SIZE     36U

extern void minios_handoff(UINT64 boot_info, UINT64 entry,
                           UINT64 page_table, UINT64 stack_top)
    __attribute__((noreturn));

static EFI_SYSTEM_TABLE *gST;
static EFI_BOOT_SERVICES *gBS;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *gConOut;

static struct minios_boot_info gBootInfo;

static UINT64 gPml5[512] __attribute__((aligned(PAGE_SIZE)));
static UINT64 gPml4[512] __attribute__((aligned(PAGE_SIZE)));
static UINT64 gPdp[512] __attribute__((aligned(PAGE_SIZE)));
static UINT64 gPd[4][512] __attribute__((aligned(PAGE_SIZE)));
static UINT64 gFbPdp[512] __attribute__((aligned(PAGE_SIZE)));
static UINT64 gFbPd[512] __attribute__((aligned(PAGE_SIZE)));
static UINT64 gCr3;

static UINT8 gStack[64 * 1024] __attribute__((aligned(16)));

static UINT64 read_cr4(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static int guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* ------------------------------------------------------------------ */
/* console and logging                                                 */
/* ------------------------------------------------------------------ */
static void con_puts(const char *s)
{
    CHAR16 line[512];
    UINTN n = 0;

    if (gConOut == (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *)0)
        return;

    while (*s != '\0' && n + 2 < (UINTN)(sizeof(line) / sizeof(line[0]))) {
        char c = *s++;
        if (c == '\n') {
            line[n++] = '\r';
            line[n++] = '\n';
        } else {
            line[n++] = (CHAR16)(unsigned char)c;
        }
    }
    line[n] = 0;
    if (n != 0)
        gConOut->OutputString(gConOut, line);
}

static void logf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    mini_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    con_puts(buf);
}

static void halt_forever(void)
{
    for (;;)
        __asm__ volatile("hlt");
}

static void panic(const char *what, EFI_STATUS status)
{
    if (gConOut != (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *)0)
        gConOut->SetAttribute(gConOut, EFI_LIGHTRED | (EFI_BLACK << 4));
    logf("\n[FATAL] %s\n", what);
    logf("        EFI status = 0x%llx\n", (unsigned long long)status);
    logf("        system halted - reset the machine\n");
    halt_forever();
}

static void copy_firmware_name(struct minios_boot_info *bi)
{
    CHAR16 *vendor = gST->FirmwareVendor;
    UINTN i = 0;

    if (vendor != (CHAR16 *)0) {
        while (vendor[i] != 0 && i + 1 < sizeof(bi->firmware)) {
            CHAR16 c = vendor[i];
            bi->firmware[i] = (c >= 32 && c < 127) ? (char)c : '?';
            i++;
        }
    }
    bi->firmware[i] = '\0';
    bi->firmware_revision = gST->FirmwareRevision;
}

/* ------------------------------------------------------------------ */
/* framebuffer                                                         */
/* ------------------------------------------------------------------ */
static void init_gop(struct minios_boot_info *bi)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = (EFI_GRAPHICS_OUTPUT_PROTOCOL *)0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    EFI_STATUS status;

    status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL,
                                 (VOID **)&gop);
    if (EFI_ERROR(status) || gop == (EFI_GRAPHICS_OUTPUT_PROTOCOL *)0 ||
        gop->Mode == (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *)0 ||
        gop->Mode->Info == (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *)0)
        panic("Graphics Output Protocol (GOP) is not available", status);

    info = gop->Mode->Info;
    bi->fb_width = info->HorizontalResolution;
    bi->fb_height = info->VerticalResolution;
    bi->fb_pitch = info->PixelsPerScanLine;
    bi->fb_format = info->PixelFormat;
    bi->fb_bpp = 32;
    bi->fb_base = gop->Mode->FrameBufferBase;
    bi->fb_size = gop->Mode->FrameBufferSize;

    switch (info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        bi->mask_red = 0x000000FF;
        bi->mask_green = 0x0000FF00;
        bi->mask_blue = 0x00FF0000;
        bi->mask_reserved = 0xFF000000;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:
        bi->mask_red = 0x00FF0000;
        bi->mask_green = 0x0000FF00;
        bi->mask_blue = 0x000000FF;
        bi->mask_reserved = 0xFF000000;
        break;
    case PixelBitMask:
        bi->mask_red = info->PixelInformation[0];
        bi->mask_green = info->PixelInformation[1];
        bi->mask_blue = info->PixelInformation[2];
        bi->mask_reserved = info->PixelInformation[3];
        break;
    default:
        bi->mask_red = 0;
        bi->mask_green = 0;
        bi->mask_blue = 0;
        bi->mask_reserved = 0;
        break;
    }

    if (bi->fb_base == 0 || bi->fb_width == 0 || bi->fb_height == 0 ||
        bi->fb_format == PixelBltOnly || bi->mask_red == 0)
        panic("GOP does not provide a linear framebuffer", EFI_ABORTED);

    logf("GOP      : %ux%u, %u bpp, pitch %u px, format %u\n",
         bi->fb_width, bi->fb_height, bi->fb_bpp, bi->fb_pitch, bi->fb_format);
    logf("Framebuf : 0x%llx (%lu MiB)\n", bi->fb_base,
         (unsigned long)(bi->fb_size >> 20));
}

/* ------------------------------------------------------------------ */
/* kernel file                                                         */
/* ------------------------------------------------------------------ */
static void load_kernel(EFI_HANDLE image)
{
    static CHAR16 *paths[] = {
        L"\\EFI\\MINIOS\\KERNEL.BIN",
        L"\\KERNEL.BIN",
    };
    static const char *ascii_paths[] = {
        "\\EFI\\MINIOS\\KERNEL.BIN",
        "\\KERNEL.BIN",
    };
    EFI_LOADED_IMAGE_PROTOCOL *li = (EFI_LOADED_IMAGE_PROTOCOL *)0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)0;
    EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)0;
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)0;
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS status;
    UINTN info_size = 0;
    UINTN pages;
    UINTN total = 0;
    UINT64 size;
    UINTN i;

    status = gBS->HandleProtocol(image, &gEfiLoadedImageProtocolGuid,
                                 (VOID **)&li);
    if (EFI_ERROR(status) || li == (EFI_LOADED_IMAGE_PROTOCOL *)0)
        panic("cannot open the LoadedImage protocol", status);
    gBootInfo.loader_base = (UINT64)(UINTN)li->ImageBase;
    gBootInfo.loader_size = li->ImageSize;

    status = gBS->HandleProtocol(li->DeviceHandle,
                                 &gEfiSimpleFileSystemProtocolGuid,
                                 (VOID **)&fs);
    if (EFI_ERROR(status) || fs == (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0)
        panic("the boot device has no SimpleFileSystem protocol", status);

    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status) || root == (EFI_FILE_PROTOCOL *)0)
        panic("cannot open the EFI System Partition", status);

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        status = root->Open(root, &file, paths[i], EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(status))
            break;
        file = (EFI_FILE_PROTOCOL *)0;
    }
    if (file == (EFI_FILE_PROTOCOL *)0) {
        root->Close(root);
        panic("\\EFI\\MINIOS\\KERNEL.BIN was not found on the ESP", status);
    }
    logf("Kernel   : %s\n", ascii_paths[i]);

    status = file->GetInfo(file, &gEfiFileInfoGuid, &info_size, NULL);
    if (info_size == 0 || (EFI_ERROR(status) && status != EFI_BUFFER_TOO_SMALL)) {
        file->Close(file);
        root->Close(root);
        panic("cannot query the size of KERNEL.BIN", status);
    }
    status = gBS->AllocatePool(EfiLoaderData, info_size, (VOID **)&fi);
    if (EFI_ERROR(status) || fi == (EFI_FILE_INFO *)0)
        panic("out of memory while reading KERNEL.BIN", status);
    status = file->GetInfo(file, &gEfiFileInfoGuid, &info_size, fi);
    if (EFI_ERROR(status)) {
        gBS->FreePool(fi);
        file->Close(file);
        root->Close(root);
        panic("cannot read KERNEL.BIN metadata", status);
    }
    size = fi->FileSize;
    gBS->FreePool(fi);

    if (size == 0 || size > KERNEL_MAX_SIZE) {
        file->Close(file);
        root->Close(root);
        panic("KERNEL.BIN has an invalid size", EFI_ABORTED);
    }

    pages = (UINTN)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    addr = (EFI_PHYSICAL_ADDRESS)KERNEL_LOAD_ADDR;
    status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(status) || addr != (EFI_PHYSICAL_ADDRESS)KERNEL_LOAD_ADDR) {
        file->Close(file);
        root->Close(root);
        panic("cannot allocate KERNEL.BIN at physical 0x100000", status);
    }

    while (total < size) {
        UINTN chunk = (UINTN)(size - total);

        status = file->Read(file, &chunk,
                            (VOID *)(UINTN)(KERNEL_LOAD_ADDR + total));
        if (EFI_ERROR(status)) {
            file->Close(file);
            root->Close(root);
            panic("read from KERNEL.BIN failed", status);
        }
        if (chunk == 0) {
            file->Close(file);
            root->Close(root);
            panic("unexpected end of KERNEL.BIN", EFI_ABORTED);
        }
        total += chunk;
    }

    file->Close(file);
    root->Close(root);

    gBootInfo.kernel_base = KERNEL_LOAD_ADDR;
    gBootInfo.kernel_size = size;
    logf("Kernel   : %llu bytes loaded at 0x%llx\n", size, KERNEL_LOAD_ADDR);
    logf("Loader   : 0x%llx (%llu bytes)\n", gBootInfo.loader_base,
         gBootInfo.loader_size);
}

/* ------------------------------------------------------------------ */
/* optional firmware blob                                              */
/* ------------------------------------------------------------------ */
static void load_firmware(EFI_HANDLE image)
{
    static CHAR16 *paths[] = {
        L"\\EFI\\MINIOS\\WIFI9.UC",
        L"\\WIFI9.UC",
    };
    static const char *ascii_paths[] = {
        "\\EFI\\MINIOS\\WIFI9.UC",
        "\\WIFI9.UC",
    };
    EFI_LOADED_IMAGE_PROTOCOL *li = (EFI_LOADED_IMAGE_PROTOCOL *)0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)0;
    EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)0;
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)0;
    EFI_PHYSICAL_ADDRESS addr = LOW_4GIB - 1;
    EFI_STATUS status;
    UINTN info_size = 0;
    UINTN pages;
    UINTN total = 0;
    UINT64 size;
    UINTN i;
    const char *name = "WIFI9.UC";
    UINTN n;

    status = gBS->HandleProtocol(image, &gEfiLoadedImageProtocolGuid,
                                 (VOID **)&li);
    if (EFI_ERROR(status) || li == (EFI_LOADED_IMAGE_PROTOCOL *)0) {
        logf("Firmware : no LoadedImage protocol, skipping\n");
        return;
    }
    status = gBS->HandleProtocol(li->DeviceHandle,
                                 &gEfiSimpleFileSystemProtocolGuid,
                                 (VOID **)&fs);
    if (EFI_ERROR(status) || fs == (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0) {
        logf("Firmware : no SimpleFileSystem, skipping\n");
        return;
    }
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status) || root == (EFI_FILE_PROTOCOL *)0) {
        logf("Firmware : cannot open ESP, skipping\n");
        return;
    }
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        status = root->Open(root, &file, paths[i], EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(status))
            break;
        file = (EFI_FILE_PROTOCOL *)0;
    }
    if (file == (EFI_FILE_PROTOCOL *)0) {
        root->Close(root);
        logf("Firmware : WIFI9.UC not found (PCI probe only)\n");
        return;
    }
    logf("Firmware : %s\n", ascii_paths[i]);

    status = file->GetInfo(file, &gEfiFileInfoGuid, &info_size, NULL);
    if (info_size == 0 ||
        (EFI_ERROR(status) && status != EFI_BUFFER_TOO_SMALL)) {
        file->Close(file);
        root->Close(root);
        logf("Firmware : cannot query size, skipping\n");
        return;
    }
    status = gBS->AllocatePool(EfiLoaderData, info_size, (VOID **)&fi);
    if (EFI_ERROR(status) || fi == (EFI_FILE_INFO *)0) {
        file->Close(file);
        root->Close(root);
        logf("Firmware : out of memory, skipping\n");
        return;
    }
    status = file->GetInfo(file, &gEfiFileInfoGuid, &info_size, fi);
    if (EFI_ERROR(status)) {
        gBS->FreePool(fi);
        file->Close(file);
        root->Close(root);
        logf("Firmware : cannot read metadata, skipping\n");
        return;
    }
    size = fi->FileSize;
    gBS->FreePool(fi);

    if (size == 0 || size > FW_MAX_SIZE) {
        file->Close(file);
        root->Close(root);
        logf("Firmware : invalid size %llu, skipping\n", size);
        return;
    }

    pages = (UINTN)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(status) || addr == 0) {
        file->Close(file);
        root->Close(root);
        logf("Firmware : no buffer below 4 GiB, skipping\n");
        return;
    }
    if (addr >= LOW_4GIB || size > LOW_4GIB - addr) {
        file->Close(file);
        root->Close(root);
        gBS->FreePages(addr, pages);
        logf("Firmware : buffer is not below 4 GiB, skipping\n");
        return;
    }

    while (total < size) {
        UINTN chunk = (UINTN)(size - total);

        status = file->Read(file, &chunk, (VOID *)(UINTN)(addr + total));
        if (EFI_ERROR(status) || chunk == 0) {
            file->Close(file);
            root->Close(root);
            gBS->FreePages(addr, pages);
            logf("Firmware : read failed, skipping\n");
            return;
        }
        total += chunk;
    }
    file->Close(file);
    root->Close(root);

    gBootInfo.fw_base = (UINT64)addr;
    gBootInfo.fw_size = size;
    n = 0;
    while (name[n] != '\0' && n + 1 < sizeof(gBootInfo.fw_name)) {
        gBootInfo.fw_name[n] = name[n];
        n++;
    }
    gBootInfo.fw_name[n] = '\0';
    logf("Firmware : 0x%llx, %llu bytes\n", (UINT64)addr, size);
}

/* ------------------------------------------------------------------ */
/* ACPI RSDP                                                           */
/* ------------------------------------------------------------------ */
static void copy_acpi_rsdp(void)
{
    EFI_CONFIGURATION_TABLE *ct = gST->ConfigurationTable;
    VOID *rsdp = (VOID *)0;
    UINTN i;
    char sig[9];

    if (ct == (EFI_CONFIGURATION_TABLE *)0)
        return;

    for (i = 0; i < gST->NumberOfTableEntries; i++) {
        if (guid_equal(&ct[i].VendorGuid, &gEfiAcpi20TableGuid) ||
            guid_equal(&ct[i].VendorGuid, &gEfiAcpiTableGuid)) {
            rsdp = ct[i].VendorTable;
            break;
        }
    }
    if (rsdp == (VOID *)0) {
        logf("ACPI RSDP: not found\n");
        return;
    }

    gBootInfo.acpi_rsdp = (UINT64)(UINTN)rsdp;
    gBootInfo.acpi_rsdp_size = RSDP_COPY_SIZE;
    memcpy(gBootInfo.acpi_rsdp_copy, rsdp, RSDP_COPY_SIZE);
    memcpy(sig, rsdp, 8);
    sig[8] = '\0';
    logf("ACPI RSDP: 0x%llx sig %s rev %u\n", gBootInfo.acpi_rsdp, sig,
         (unsigned int)((const UINT8 *)rsdp)[15]);
}

/* ------------------------------------------------------------------ */
/* memory map                                                          */
/* ------------------------------------------------------------------ */
static UINT64 usable_ram_from_map(VOID *map, UINTN size, UINTN desc_size)
{
    UINT64 total = 0;
    UINTN off = 0;

    while (off + desc_size <= size) {
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + off);

        if (d->Type == EfiConventionalMemory ||
            d->Type == EfiLoaderCode ||
            d->Type == EfiLoaderData)
            total += d->NumberOfPages * PAGE_SIZE;
        off += desc_size;
    }
    return total;
}

static UINT64 probe_memory(void)
{
    UINTN size = 0;
    UINTN key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    EFI_STATUS status;
    VOID *map = NULL;
    UINT64 total = 0;

    status = gBS->GetMemoryMap(&size, NULL, &key, &desc_size, &desc_version);
    if (desc_size == 0 ||
        (EFI_ERROR(status) && status != EFI_BUFFER_TOO_SMALL))
        return 0;

    size += desc_size * 8;
    status = gBS->AllocatePool(EfiLoaderData, size, &map);
    if (EFI_ERROR(status) || map == NULL)
        return 0;

    status = gBS->GetMemoryMap(&size, map, &key, &desc_size, &desc_version);
    if (!EFI_ERROR(status))
        total = usable_ram_from_map(map, size, desc_size);

    gBS->FreePool(map);
    return total;
}

/*
 * ExitBootServices needs the map key from the most recent GetMemoryMap, and
 * nothing may allocate between the two calls.  Loop in case the map grows.
 */
static void exit_boot_services(EFI_HANDLE image)
{
    int attempt;

    for (attempt = 0; attempt < 8; attempt++) {
        UINTN size = 0;
        UINTN key = 0;
        UINTN desc_size = 0;
        UINT32 desc_version = 0;
        EFI_STATUS status;
        EFI_PHYSICAL_ADDRESS map_addr = LOW_4GIB - 1;
        UINTN map_pages;
        EFI_MEMORY_DESCRIPTOR *map;

        status = gBS->GetMemoryMap(&size, NULL, &key, &desc_size,
                                   &desc_version);
        if (desc_size == 0 ||
            (EFI_ERROR(status) && status != EFI_BUFFER_TOO_SMALL))
            panic("GetMemoryMap failed before ExitBootServices", status);

        size += desc_size * 8;
        map_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                    map_pages, &map_addr);
        if (EFI_ERROR(status) || map_addr == 0 ||
            map_addr + (UINT64)map_pages * PAGE_SIZE > LOW_4GIB)
            panic("cannot allocate the final memory map below 4 GiB",
                  status);

        map = (EFI_MEMORY_DESCRIPTOR *)(UINTN)map_addr;
        logf("MemMap   : buffer 0x%llx, %llu bytes (desc %u)\n",
             (UINT64)map_addr, (UINT64)map_pages * PAGE_SIZE,
             (UINT32)desc_size);
        status = gBS->GetMemoryMap(&size, map, &key, &desc_size,
                                   &desc_version);
        if (EFI_ERROR(status)) {
            gBS->FreePages(map_addr, map_pages);
            if (status == EFI_BUFFER_TOO_SMALL)
                continue;
            panic("GetMemoryMap failed before ExitBootServices", status);
        }

        gBootInfo.memory_map = (UINT64)map_addr;
        gBootInfo.memory_map_size = size;
        gBootInfo.memory_map_desc_size = (UINT32)desc_size;
        gBootInfo.memory_map_desc_version = desc_version;

        status = gBS->ExitBootServices(image, key);
        if (!EFI_ERROR(status)) {
            gConOut = (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *)0;
            return;
        }

        gBootInfo.memory_map = 0;
        gBootInfo.memory_map_size = 0;
        gBootInfo.memory_map_desc_size = 0;
        gBootInfo.memory_map_desc_version = 0;
        gBS->FreePages(map_addr, map_pages);
    }
    panic("ExitBootServices failed after several retries", EFI_ABORTED);
}

/* ------------------------------------------------------------------ */
/* page tables                                                         */
/* ------------------------------------------------------------------ */
static void setup_page_tables(UINT64 loader_base, UINT64 loader_size)
{
    UINT64 cr4 = read_cr4();
    UINT64 fb_base = gBootInfo.fb_base;
    UINT64 fb_end = fb_base + gBootInfo.fb_size;
    UINT64 i;
    UINT64 j;

    if (loader_base + loader_size > LOW_4GIB)
        panic("loader is above 4 GiB; unsupported", EFI_ABORTED);

    memset(gPml5, 0, sizeof(gPml5));
    memset(gPml4, 0, sizeof(gPml4));
    memset(gPdp, 0, sizeof(gPdp));
    memset(gFbPdp, 0, sizeof(gFbPdp));
    memset(gFbPd, 0, sizeof(gFbPd));

    /* Low memory: identity map 0..4 GiB with 2 MiB pages. */
    for (i = 0; i < 4; i++) {
        memset(gPd[i], 0, sizeof(gPd[i]));
        gPdp[i] = (UINT64)(UINTN)gPd[i] | 0x3;   /* present + writable */
        for (j = 0; j < 512; j++)
            gPd[i][j] = ((i << 30) + (j << 21)) | 0x83;
    }
    gPml4[0] = (UINT64)(UINTN)gPdp | 0x3;
    gPml5[0] = (UINT64)(UINTN)gPml4 | 0x3;

    /*
     * A 64-bit GOP framebuffer BAR can sit far above RAM, for example at
     * 0x4000000000 (256 GiB) on the Lenovo XiaoXin Air 14 2020.  Do not map
     * all the way up to it; map just the 1 GiB region that contains it.
     */
    if (fb_base >= LOW_4GIB) {
        UINT64 region = fb_base & ~(REGION_1GIB - 1);
        UINT64 pml4_index = (fb_base >> 39) & 0x1FF;
        UINT64 pdp_index = (fb_base >> 30) & 0x1FF;

        if (fb_end > region + REGION_1GIB)
            panic("framebuffer spans more than one 1 GiB region", EFI_ABORTED);
        if ((cr4 & (1ULL << 12)) != 0 && (fb_base >> 48) != 0)
            panic("framebuffer is outside the PML5[0] range", EFI_ABORTED);

        /* PCD keeps framebuffer writes uncached on a 64-bit MMIO BAR. */
        for (j = 0; j < 512; j++)
            gFbPd[j] = (region + (j << 21)) | 0x93;
        if (pml4_index == 0) {
            gPdp[pdp_index] = (UINT64)(UINTN)gFbPd | 0x3;
        } else {
            gPml4[pml4_index] = (UINT64)(UINTN)gFbPdp | 0x3;
            gFbPdp[pdp_index] = (UINT64)(UINTN)gFbPd | 0x3;
        }
        logf("Paging   : low 4 GiB + framebuffer region 0x%llx-0x%llx\n",
             region, region + REGION_1GIB);
    } else {
        if (fb_end > LOW_4GIB)
            panic("framebuffer crosses the 4 GiB boundary", EFI_ABORTED);
        logf("Paging   : identity 0-4 GiB, 2 MiB pages\n");
    }

    gCr3 = (cr4 & (1ULL << 12)) != 0
               ? (UINT64)(UINTN)gPml5
               : (UINT64)(UINTN)gPml4;
    logf("Paging   : %s, CR3=0x%llx\n",
         (cr4 & (1ULL << 12)) != 0 ? "5-level" : "4-level", gCr3);
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    serial_init();

    gST = st;
    gBS = (st != (EFI_SYSTEM_TABLE *)0) ? st->BootServices
                                        : (EFI_BOOT_SERVICES *)0;
    gConOut = (st != (EFI_SYSTEM_TABLE *)0)
                  ? st->ConOut
                  : (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *)0;
    if (gBS == (EFI_BOOT_SERVICES *)0 ||
        gConOut == (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *)0)
        return EFI_LOAD_ERROR;

    gConOut->SetAttribute(gConOut, EFI_LIGHTGRAY | (EFI_BLACK << 4));
    gConOut->ClearScreen(gConOut);

    memset(&gBootInfo, 0, sizeof(gBootInfo));
    gBootInfo.magic = MINIOS_BOOT_MAGIC;
    gBootInfo.version = MINIOS_BOOT_VERSION;
    gBootInfo.runtime_services = (UINT64)(UINTN)gST->RuntimeServices;
    copy_firmware_name(&gBootInfo);

    logf("Mini-OS UEFI Loader\n");
    logf("===================\n\n");
    logf("Firmware : %s (rev 0x%08x)\n", gBootInfo.firmware,
         gBootInfo.firmware_revision);

    if (gBS->SetWatchdogTimer != 0)
        gBS->SetWatchdogTimer(0, 0, 0, NULL);

    init_gop(&gBootInfo);
    load_kernel(image);
    load_firmware(image);
    copy_acpi_rsdp();

    gBootInfo.usable_ram = probe_memory();
    if (gBootInfo.usable_ram != 0)
        logf("Usable   : %llu MiB\n", gBootInfo.usable_ram >> 20);
    else
        logf("Usable   : unknown\n");

    setup_page_tables(gBootInfo.loader_base, gBootInfo.loader_size);
    logf("BootInfo : 0x%llx, magic 0x%llx\n",
         (unsigned long long)(UINTN)&gBootInfo, gBootInfo.magic);

    logf("\nExiting boot services...\n");
    exit_boot_services(image);

    /* From here on the firmware console and boot services are gone. */
    serial_puts("[loader] boot services exited\n");
    serial_puts("[loader] handing off to kernel at 0x100000\n");

    minios_handoff((UINT64)(UINTN)&gBootInfo,
                   (UINT64)KERNEL_LOAD_ADDR,
                   gCr3,
                   (UINT64)(UINTN)(gStack + sizeof(gStack)));

    return EFI_SUCCESS; /* not reached */
}
