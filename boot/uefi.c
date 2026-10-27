#include <stdint.h>
#include "../include/boot_info.h"
#include "../include/boot_validate.h"
#include "../arch/x86_64/cpu.h"
#include "../drivers/serial.h"
#include "../kernel/api.h"

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint16_t CHAR16;
#define EFI_SUCCESS 0ULL
#define EFI_BUFFER_TOO_SMALL ((1ULL << 63) | 5ULL)
#define EFI_INVALID_PARAMETER ((1ULL << 63) | 2ULL)
#define EFI_ERROR(s) (((s) >> 63) != 0)
#define EFI_LOADER_DATA 2U
#define EFI_PAGE_SIZE 4096ULL

typedef struct { uint64_t signature; uint32_t revision, header_size, crc32, reserved; } EFI_TABLE_HEADER;
typedef struct EFI_TEXT_OUTPUT EFI_TEXT_OUTPUT;
struct EFI_TEXT_OUTPUT {
    EFI_STATUS (*reset)(EFI_TEXT_OUTPUT *, uint8_t);
    EFI_STATUS (*output_string)(EFI_TEXT_OUTPUT *, CHAR16 *);
    void *rest[7];
};
typedef struct {
    uint32_t version, horizontal_resolution, vertical_resolution, pixel_format;
    uint32_t pixel_info[4], pixels_per_scanline;
} GOP_MODE_INFO;
typedef struct {
    uint32_t max_mode, mode;
    GOP_MODE_INFO *info;
    uint64_t info_size, framebuffer_base, framebuffer_size;
} GOP_MODE;
typedef struct {
    void *query_mode, *set_mode, *blt;
    GOP_MODE *mode;
} GOP_PROTOCOL;
typedef struct {
    EFI_TABLE_HEADER header;
    CHAR16 *firmware_vendor;
    uint32_t firmware_revision;
    EFI_HANDLE con_in_handle;
    void *con_in;
    EFI_HANDLE con_out_handle;
    EFI_TEXT_OUTPUT *con_out;
    EFI_HANDLE stderr_handle;
    void *stderr;
    void *runtime_services;
    void *boot_services;
    uint64_t table_count;
    void *configuration_table;
} EFI_SYSTEM_TABLE;
typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } EFI_GUID;

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_LOCATE_PROTOCOL)(EFI_GUID *, void *, void **);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GET_MEMORY_MAP)(uint64_t *, EFI_MEMORY_DESCRIPTOR *, uint64_t *, uint64_t *, uint32_t *);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_ALLOCATE_POOL)(uint32_t, uint64_t, void **);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FREE_POOL)(void *);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE, uint64_t);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_SET_WATCHDOG)(uint64_t, uint64_t, uint64_t, CHAR16 *);

static uint64_t read_tsc(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static void firmware_message(EFI_SYSTEM_TABLE *st, const char *ascii, serial_level_t level) {
    kernel_early_log(level, ascii);
    if (!st || !st->con_out || !st->con_out->output_string) return;
    CHAR16 wide[160];
    uint32_t i = 0;
    while (ascii[i] && i < 157) { wide[i] = (uint8_t)ascii[i]; ++i; }
    wide[i++] = '\r'; wide[i++] = '\n'; wide[i] = 0;
    st->con_out->output_string(st->con_out, wide);
}

static EFI_STATUS boot_error(EFI_SYSTEM_TABLE *st, const char *reason, EFI_STATUS status) {
    firmware_message(st, reason, SERIAL_ERROR);
    return status ? status : EFI_BUFFER_TOO_SMALL;
}

static EFI_STATUS boot_error_after_exit(EFI_STATUS status, const char *reason) {
    /* Do not call UEFI console protocols after ExitBootServices has failed. */
    kernel_early_log(SERIAL_ERROR, reason);
    return status;
}

EFI_STATUS __attribute__((ms_abi)) efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    static EFI_GUID gop_guid = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
    if (!st || !st->boot_services) return EFI_BUFFER_TOO_SMALL;

    serial_init();
    firmware_message(st, "BOOT: UEFI application entry.", SERIAL_INFO);
    uint8_t *bs = (uint8_t *)st->boot_services;
    void **slot = (void **)(bs + sizeof(EFI_TABLE_HEADER));
    EFI_LOCATE_PROTOCOL locate = (EFI_LOCATE_PROTOCOL)slot[37];
    if (!locate) return boot_error(st, "BOOT ERROR: LocateProtocol unavailable.", 0);

    GOP_PROTOCOL *gop = 0;
    EFI_STATUS status = locate(&gop_guid, 0, (void **)&gop);
    if (EFI_ERROR(status) || !gop || !gop->mode || !gop->mode->info)
        return boot_error(st, "BOOT ERROR: GOP protocol unavailable.", status);
    GOP_MODE_INFO *mode = gop->mode->info;
    if (mode->pixel_format > 1)
        return boot_error(st, "BOOT ERROR: GOP pixel format is unsupported (expected RGB or BGR).", 0);
    if (mode->horizontal_resolution < 320 || mode->vertical_resolution < 240 ||
        mode->pixels_per_scanline < mode->horizontal_resolution || !gop->mode->framebuffer_base ||
        gop->mode->framebuffer_size > UINT64_MAX - gop->mode->framebuffer_base)
        return boot_error(st, "BOOT ERROR: GOP framebuffer geometry is invalid.", 0);
    uint64_t pixels = (uint64_t)mode->pixels_per_scanline * mode->vertical_resolution;
    if (pixels > UINT64_MAX / 4 || pixels * 4 > gop->mode->framebuffer_size)
        return boot_error(st, "BOOT ERROR: GOP framebuffer range is too small.", 0);

    boot_info_t info = {0};
    info.version = 1;
    info.framebuffer_width = mode->horizontal_resolution;
    info.framebuffer_height = mode->vertical_resolution;
    info.framebuffer_stride = mode->pixels_per_scanline;
    info.pixel_format = mode->pixel_format;
    info.framebuffer_base = gop->mode->framebuffer_base;
    info.framebuffer_size = gop->mode->framebuffer_size;
    cpu_identify(&info);
    firmware_message(st, "BOOT: GOP and CPU metadata captured.", SERIAL_INFO);

    EFI_GET_MEMORY_MAP getmap;
    EFI_ALLOCATE_POOL alloc;
    EFI_FREE_POOL freepool;
    EFI_EXIT_BOOT_SERVICES exitbs;
    EFI_SET_WATCHDOG watchdog;
    getmap = (EFI_GET_MEMORY_MAP)slot[4];
    alloc = (EFI_ALLOCATE_POOL)slot[5];
    freepool = (EFI_FREE_POOL)slot[6];
    exitbs = (EFI_EXIT_BOOT_SERVICES)slot[26];
    watchdog = (EFI_SET_WATCHDOG)slot[29];
    if (!getmap || !alloc || !exitbs)
        return boot_error(st, "BOOT ERROR: Required memory boot services unavailable.", 0);
    if (watchdog) watchdog(0, 0, 0, 0);

    uint64_t required = 0, key = 0, descriptor_size = 0;
    uint32_t descriptor_version = 0;
    status = getmap(&required, 0, &key, &descriptor_size, &descriptor_version);
    if (status != EFI_BUFFER_TOO_SMALL || descriptor_version != 1 ||
        descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        descriptor_size > (UINT64_MAX - required) / 32)
        return boot_error(st, "BOOT ERROR: Could not size a supported UEFI memory map.", status);

    uint64_t capacity = required + descriptor_size * 32;
    void *map = 0;
    status = alloc(EFI_LOADER_DATA, capacity, &map);
    if (EFI_ERROR(status) || !map)
        return boot_error(st, "BOOT ERROR: Could not allocate memory-map storage.", status);
    firmware_message(st, "BOOT: memory-map buffer allocated; preparing handoff.", SERIAL_INFO);

    uint64_t map_size = 0;
    int exited = 0;
    uint32_t growths = 0;
    for (uint32_t attempt = 0; attempt < 4; ++attempt) {
        map_size = capacity;
        status = getmap(&map_size, (EFI_MEMORY_DESCRIPTOR *)map, &key, &descriptor_size, &descriptor_version);
        if (status == EFI_BUFFER_TOO_SMALL) {
            if (growths++ >= 2 || !freepool || descriptor_version != 1 ||
                descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
                descriptor_size > (UINT64_MAX - map_size) / 32)
                return boot_error(st, "BOOT ERROR: Memory map grew beyond the reserved buffer.", status);
            EFI_STATUS free_status = freepool(map);
            if (EFI_ERROR(free_status))
                return boot_error(st, "BOOT ERROR: Could not release old memory-map storage.", free_status);
            capacity = map_size + descriptor_size * 32;
            status = alloc(EFI_LOADER_DATA, capacity, &map);
            if (EFI_ERROR(status) || !map)
                return boot_error(st, "BOOT ERROR: Could not grow memory-map storage.", status);
            continue;
        }
        if (EFI_ERROR(status) || descriptor_version != 1 ||
            descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) || map_size % descriptor_size)
            return boot_error(st, "BOOT ERROR: UEFI returned an invalid memory map.", status);

        info.memory_map = (const EFI_MEMORY_DESCRIPTOR *)map;
        info.memory_map_size = map_size;
        info.memory_descriptor_size = descriptor_size;
        info.memory_descriptor_count = map_size / descriptor_size;
        info.conventional_memory_bytes = 0;
        for (uint64_t i = 0; i < info.memory_descriptor_count; ++i) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + i * descriptor_size);
            if (d->type == EFI_MEMORY_CONVENTIONAL &&
                d->pages <= (UINT64_MAX - info.conventional_memory_bytes) / EFI_PAGE_SIZE)
                info.conventional_memory_bytes += d->pages * EFI_PAGE_SIZE;
        }

        if (!boot_info_validate(&info))
            return boot_error(st, "BOOT ERROR: Captured boot information failed validation.", 0);

        /* OutputString may allocate firmware memory and invalidate this map key. */
        kernel_early_log(SERIAL_INFO, "BOOT: exiting UEFI boot services.");
        status = exitbs(image, key);
        if (!EFI_ERROR(status)) { exited = 1; break; }
        if (status != EFI_INVALID_PARAMETER)
            return boot_error_after_exit(status, "BOOT ERROR: ExitBootServices failed for a reason other than a stale map key.");
        kernel_early_log(SERIAL_WARN, "BOOT: memory-map key changed; retrying handoff.");
    }
    if (!exited) return boot_error_after_exit(status, "BOOT ERROR: ExitBootServices failed after retries.");

    /* Do not let firmware-era interrupts run after boot services have ended. */
    __asm__ volatile("cli" ::: "memory");
    kernel_set_boot_tsc(read_tsc());
    kernel_main(&info);
    return EFI_SUCCESS;
}
