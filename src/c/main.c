#define EFIAPI __attribute__((ms_abi))

typedef unsigned char UINT8;
typedef unsigned short CHAR16;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef unsigned long long UINTN;
typedef long long EFI_STATUS;
typedef void *EFI_HANDLE;

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct _EFT_SIMPLE_TEXT_INPUT_PROTOCOL;
struct _EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareVersion;
    UINT32 __pad;

    EFI_HANDLE ConsoleInHandle;
    struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;

    EFI_HANDLE ConsoleOutHandle;
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;

    void *StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    struct _EFI_BOOT_SERVICES *BootServices;
} EFI_SYSTEM_TABLE;


typedef struct {
    UINT32 Type;
    UINT32 Pad;
    UINT64 PhysicalStart;
    UINT64 VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;


#define EfiConventionalMemory 7

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey,
    UINTN *DescriptorSize,
    UINT32 *DescriptorVersion
);


typedef struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    void *AllocatePages;
    void *FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    void *AllocatePool;
    void *FreePool;

    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    void *HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstalLConfigurationTable;

    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    void *ExitBootServices;

    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;

    void *ConnectController;
    void *DisconnectController;

    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;

    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    void *LocateProtocol;
} EFI_BOOT_SERVICES;


typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);

typedef struct {
    unsigned short ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_TEXT_STRING OutputString;
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY)(struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);

typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    void *Reset;
    EFI_INPUT_READ_KEY ReadKeyStroke;
    void *WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;


char memory_map_buffer[1024 * 256];


void UINT64ToHexStr(UINT64 val, CHAR16 *str) {
    char hex[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        str[i + 2] = hex[val & 0xF];
        val >>= 4;
    }
    str[0] = L'0';
    str[1] = L'x';
    str[18] = L'\0';
}

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    // clear screen
    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Hello from isikiOS\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Memory checking...\r\n");


    UINTN memory_map_size = 0;
    EFI_MEMORY_DESCRIPTOR *memory_map = (void *)0;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;
    EFI_STATUS status;
    UINT64 max_free_size = 0;
    UINT64 heap_start = 0;

    status = SystemTable->BootServices->GetMemoryMap(
        &memory_map_size, memory_map, &map_key, &descriptor_size, &descriptor_version
    );

    memory_map = (EFI_MEMORY_DESCRIPTOR *)memory_map_buffer;
    memory_map_size = sizeof(memory_map_buffer);

    status = SystemTable->BootServices->GetMemoryMap(
        &memory_map_size, memory_map, &map_key, &descriptor_size, &descriptor_version
    );

    if (status == 0) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Success to get Memory Map!\r\n");

        UINTN entries = memory_map_size / descriptor_size;

        for (UINTN i = 0; i < entries; i++) {
            EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((char *)memory_map + (i * descriptor_size));

            if (desc->Type == EfiConventionalMemory) {
                UINT64 size = desc->NumberOfPages * 4096;
                if (size > max_free_size) {
                    max_free_size = size;
                    heap_start = desc->PhysicalStart;
                }
            }
        }

        CHAR16 hex_addr[20];
        CHAR16 hex_size[20];
        UINT64ToHexStr(heap_start, hex_addr);
        UINT64ToHexStr(max_free_size, hex_size);

        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"OS Heap Target Address: ");
        SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_addr);
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");

        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Available Heap Size: ");
        SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_size);
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
    } else {
        CHAR16 hex_status[20];
        CHAR16 hex_needed[20];
        UINT64ToHexStr((UINT64)status, hex_status);
        UINT64ToHexStr((UINT64)memory_map_size, hex_needed);

        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Failed to get Memory Map. Status: ");
        SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_status);
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L" / Needed SIze: ");
        SystemTable->ConOut->OutputString(SystemTable->ConOut, hex_needed);
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n");
        for (;;) {
        }
    }
   

    for (;;) {
    }
}
