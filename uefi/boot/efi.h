/*
 * efi.h - minimal, self-contained UEFI definitions for the Mini-OS loader.
 *
 * Only what the loader touches is used, but the tables are declared in the
 * order given by the UEFI specification so that offsets are correct.  Every
 * function the firmware calls is EFIAPI (Microsoft x64 ABI); loader-internal
 * code uses the normal SysV ABI.
 */
#ifndef MINIOS_EFI_H
#define MINIOS_EFI_H

#define EFIAPI __attribute__((ms_abi))

typedef unsigned char       BOOLEAN;
typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long long  UINT64;
typedef signed char         INT8;
typedef signed short        INT16;
typedef signed int          INT32;
typedef signed long long    INT64;
typedef unsigned long long  UINTN;
typedef signed long long    INTN;
typedef unsigned short      CHAR16;
typedef void                VOID;

typedef UINTN   EFI_STATUS;
typedef UINTN   EFI_TPL;
typedef VOID   *EFI_HANDLE;
typedef VOID   *EFI_EVENT;
typedef UINT64  EFI_PHYSICAL_ADDRESS;
typedef UINT64  EFI_VIRTUAL_ADDRESS;

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ------------------------------------------------------------------ */
/* status codes                                                        */
/* ------------------------------------------------------------------ */
#define EFI_SUCCESS           0u
#define EFI_LOAD_ERROR        (1ULL << 63 | 1)
#define EFI_INVALID_PARAMETER (1ULL << 63 | 2)
#define EFI_BUFFER_TOO_SMALL  (1ULL << 63 | 5)
#define EFI_NOT_FOUND         (1ULL << 63 | 14)
#define EFI_ABORTED           (1ULL << 63 | 21)

#define EFI_ERROR(s) (((s) & (1ULL << 63)) != 0)

/* ------------------------------------------------------------------ */
/* memory types / allocation types                                     */
/* ------------------------------------------------------------------ */
typedef enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode = 1,
    EfiLoaderData = 2,
    EfiBootServicesCode = 3,
    EfiBootServicesData = 4,
    EfiRuntimeServicesCode = 5,
    EfiRuntimeServicesData = 6,
    EfiConventionalMemory = 7,
    EfiUnusableMemory = 8,
    EfiACPIReclaimMemory = 9,
    EfiACPIMemoryNVS = 10,
    EfiMemoryMappedIO = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode = 13,
    EfiPersistentMemory = 14,
    EfiMaxMemoryType = 15
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages = 0,
    AllocateMaxAddress = 1,
    AllocateAddress = 2,
    MaxAllocateType = 3
} EFI_ALLOCATE_TYPE;

typedef struct {
    UINT32 Type;
    UINT32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ------------------------------------------------------------------ */
/* simple text output                                                  */
/* ------------------------------------------------------------------ */
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
    INT32   MaxMode;
    INT32   Mode;
    INT32   Attribute;
    INT32   CursorColumn;
    INT32   CursorRow;
    BOOLEAN CursorVisible;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                               BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *OutputString)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      CHAR16 *String);
    EFI_STATUS (EFIAPI *TestString)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                    CHAR16 *String);
    EFI_STATUS (EFIAPI *QueryMode)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                   UINTN ModeNumber, UINTN *Columns, UINTN *Rows);
    EFI_STATUS (EFIAPI *SetMode)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                 UINTN ModeNumber);
    EFI_STATUS (EFIAPI *SetAttribute)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      UINTN Attribute);
    EFI_STATUS (EFIAPI *ClearScreen)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    EFI_STATUS (EFIAPI *SetCursorPosition)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                           UINTN Column, UINTN Row);
    EFI_STATUS (EFIAPI *EnableCursor)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      BOOLEAN Visible);
    EFI_SIMPLE_TEXT_OUTPUT_MODE *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* text attribute = background << 4 | foreground */
#define EFI_BLACK        0x00
#define EFI_BLUE         0x01
#define EFI_GREEN        0x02
#define EFI_CYAN         0x03
#define EFI_RED          0x04
#define EFI_MAGENTA      0x05
#define EFI_BROWN        0x06
#define EFI_LIGHTGRAY    0x07
#define EFI_DARKGRAY     0x08
#define EFI_LIGHTBLUE    0x09
#define EFI_LIGHTGREEN   0x0A
#define EFI_LIGHTCYAN    0x0B
#define EFI_LIGHTRED     0x0C
#define EFI_LIGHTMAGENTA 0x0D
#define EFI_YELLOW       0x0E
#define EFI_WHITE        0x0F

/* ------------------------------------------------------------------ */
/* boot services                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_TPL (EFIAPI *RaiseTPL)(EFI_TPL NewTpl);
    VOID    (EFIAPI *RestoreTPL)(EFI_TPL OldTpl);
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type,
                                       EFI_MEMORY_TYPE MemoryType,
                                       UINTN Pages,
                                       EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize,
                                      EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                      UINTN *MapKey,
                                      UINTN *DescriptorSize,
                                      UINT32 *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType,
                                      UINTN Size, VOID **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(VOID *Buffer);
    VOID *CreateEvent;
    VOID *SetTimer;
    VOID *WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    VOID *CheckEvent;
    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle,
                                        EFI_GUID *Protocol,
                                        VOID **Interface);
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    VOID *LocateHandle;
    VOID *LocateDevicePath;
    VOID *InstallConfigurationTable;
    VOID *LoadImage;
    VOID *StartImage;
    VOID *Exit;
    VOID *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    VOID *GetNextMonotonicCount;
    VOID *Stall;
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout,
                                          UINT64 WatchdogCode,
                                          UINTN DataSize,
                                          CHAR16 *WatchdogData);
    VOID *ConnectController;
    VOID *DisconnectController;
    VOID *OpenProtocol;
    VOID *CloseProtocol;
    VOID *OpenProtocolInformation;
    VOID *ProtocolsPerHandle;
    VOID *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol,
                                        VOID *Registration,
                                        VOID **Interface);
    VOID *InstallMultipleProtocolInterfaces;
    VOID *UninstallMultipleProtocolInterfaces;
    VOID *CalculateCrc32;
    VOID *CopyMem;
    VOID *SetMem;
    VOID *CreateEventEx;
} EFI_BOOT_SERVICES;

/* ------------------------------------------------------------------ */
/* system table                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32  FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    VOID      *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    VOID      *StdErr;
    VOID      *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN      NumberOfTableEntries;
    VOID      *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ------------------------------------------------------------------ */
/* loaded image protocol                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    VOID *FilePath;
    VOID *Reserved;
    UINT32 LoadOptionsSize;
    VOID *LoadOptions;
    VOID *ImageBase;
    UINT64 ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    EFI_STATUS (EFIAPI *Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

/* ------------------------------------------------------------------ */
/* simple file system                                                  */
/* ------------------------------------------------------------------ */
struct _EFI_FILE_PROTOCOL;

typedef struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(struct _EFI_FILE_PROTOCOL *This,
                              struct _EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName,
                              UINT64 OpenMode,
                              UINT64 Attributes);
    EFI_STATUS (EFIAPI *Close)(struct _EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(struct _EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(struct _EFI_FILE_PROTOCOL *This,
                              UINTN *BufferSize, VOID *Buffer);
    EFI_STATUS (EFIAPI *Write)(struct _EFI_FILE_PROTOCOL *This,
                               UINTN *BufferSize, VOID *Buffer);
    EFI_STATUS (EFIAPI *GetPosition)(struct _EFI_FILE_PROTOCOL *This,
                                     UINT64 *Position);
    EFI_STATUS (EFIAPI *SetPosition)(struct _EFI_FILE_PROTOCOL *This,
                                     UINT64 Position);
    EFI_STATUS (EFIAPI *GetInfo)(struct _EFI_FILE_PROTOCOL *This,
                                 EFI_GUID *InformationType,
                                 UINTN *BufferSize, VOID *Buffer);
    EFI_STATUS (EFIAPI *SetInfo)(struct _EFI_FILE_PROTOCOL *This,
                                 EFI_GUID *InformationType,
                                 UINTN BufferSize, VOID *Buffer);
    EFI_STATUS (EFIAPI *Flush)(struct _EFI_FILE_PROTOCOL *This);
} EFI_FILE_PROTOCOL;

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                    EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

/* ------------------------------------------------------------------ */
/* graphics output protocol                                            */
/* ------------------------------------------------------------------ */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask = 2,
    PixelBltOnly = 3,
    PixelFormatMax = 4
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    UINT32 PixelInformation[4];
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN  SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN  FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    EFI_STATUS (EFIAPI *QueryMode)(VOID *This, UINT32 ModeNumber,
                                   UINTN *SizeOfInfo,
                                   EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (EFIAPI *SetMode)(VOID *This, UINT32 ModeNumber);
    VOID *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ------------------------------------------------------------------ */
/* protocol GUIDs                                                      */
/* ------------------------------------------------------------------ */
static EFI_GUID gEfiGraphicsOutputProtocolGuid =
    { 0x9042a9de, 0x23dc, 0x4a38,
      { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };

static EFI_GUID gEfiLoadedImageProtocolGuid =
    { 0x5b1b31a1, 0x9562, 0x11d2,
      { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

static EFI_GUID gEfiSimpleFileSystemProtocolGuid =
    { 0x964e5b22, 0x6459, 0x11d2,
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

static EFI_GUID gEfiFileInfoGuid =
    { 0x09576e92, 0x6d3f, 0x11d2,
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

#endif /* MINIOS_EFI_H */
