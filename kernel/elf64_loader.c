#include <stddef.h>
#include <stdint.h>
#include "elf64_loader.h"

#define ELF_MAGIC0 0x7Fu
#define ELF_PT_NULL 0u
#define ELF_PT_LOAD 1u
#define ELF_PT_DYNAMIC 2u
#define ELF_PT_INTERP 3u
#define ELF_PT_NOTE 4u
#define ELF_PT_PHDR 6u
#define ELF_PT_GNU_STACK 0x6474E551u
#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_PF_R 4u
#define ELF_PF_ALLOWED (ELF_PF_X | ELF_PF_W | ELF_PF_R)
#define ELF_PAGE_SIZE UINT64_C(4096)

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t read64(const uint8_t *p) {
    return (uint64_t)read32(p) | ((uint64_t)read32(p + 4) << 32);
}
static int power_of_two(uint64_t value) { return value && !(value & (value - 1)); }
static uint64_t page_down(uint64_t value) { return value & ~(ELF_PAGE_SIZE - 1); }

elf64_status_t elf64_parse(const void *image_pointer, size_t image_size,
                           uint8_t levels, elf64_plan_t *plan) {
    if (!image_pointer || !plan || image_size < 64 || (levels != 4 && levels != 5))
        return ELF64_INVALID;
    const uint8_t *image = (const uint8_t *)image_pointer;
    if (image[0] != ELF_MAGIC0 || image[1] != 'E' || image[2] != 'L' || image[3] != 'F' ||
        image[4] != 2 || image[5] != 1 || image[6] != 1 || image[8] != 0 ||
        (image[7] != 0 && image[7] != 3)) return ELF64_INVALID;
    if (read16(image + 16) != 2 || read16(image + 18) != 62 ||
        read32(image + 20) != 1 || read32(image + 48) != 0)
        return ELF64_UNSUPPORTED;
    if (read16(image + 52) != 64 || read16(image + 54) != 56)
        return ELF64_INVALID;

    uint64_t entry = read64(image + 24);
    uint64_t phoff = read64(image + 32);
    uint16_t phnum = read16(image + 56);
    if (!phnum || phnum > 128 || phoff < 64 || (phoff & 7u) || phoff > image_size ||
        (uint64_t)phnum > ((uint64_t)image_size - phoff) / 56)
        return ELF64_INVALID;

    elf64_plan_t result;
    result.entry = entry;
    result.segment_count = 0;
    result.page_count = 0;
    int entry_is_executable = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t *ph = image + (size_t)phoff + (size_t)i * 56;
        uint32_t type = read32(ph);
        if (type != ELF_PT_LOAD) {
            if (type == ELF_PT_DYNAMIC || type == ELF_PT_INTERP || type == ELF_PT_PHDR)
                return ELF64_UNSUPPORTED;
            if (type == ELF_PT_NULL) continue;
            if (type == ELF_PT_GNU_STACK) {
                uint32_t stack_flags = read32(ph + 4);
                if ((stack_flags & ~ELF_PF_ALLOWED) || (stack_flags & ELF_PF_X))
                    return ELF64_UNSUPPORTED;
                continue;
            }
            if (type == ELF_PT_NOTE) {
                uint32_t note_flags = read32(ph + 4);
                uint64_t note_offset = read64(ph + 8);
                uint64_t note_size = read64(ph + 32);
                if ((note_flags & ~ELF_PF_ALLOWED) || note_offset > image_size ||
                    note_size > (uint64_t)image_size - note_offset) return ELF64_INVALID;
                continue;
            }
            return ELF64_UNSUPPORTED;
        }
        if (result.segment_count == ELF64_MAX_LOAD_SEGMENTS) return ELF64_UNSUPPORTED;

        uint32_t flags = read32(ph + 4);
        uint64_t file_offset = read64(ph + 8);
        uint64_t virtual_address = read64(ph + 16);
        uint64_t file_size = read64(ph + 32);
        uint64_t memory_size = read64(ph + 40);
        uint64_t alignment = read64(ph + 48);
        if (!memory_size || file_size > memory_size || (flags & ~ELF_PF_ALLOWED) ||
            !(flags & ELF_PF_R) || ((flags & (ELF_PF_W | ELF_PF_X)) == (ELF_PF_W | ELF_PF_X)))
            return ELF64_INVALID;
        if (file_offset > image_size || file_size > (uint64_t)image_size - file_offset ||
            virtual_address < ELF_PAGE_SIZE ||
            !vm_user_address_valid(virtual_address, memory_size, levels)) return ELF64_INVALID;
        if ((virtual_address & (ELF_PAGE_SIZE - 1)) != (file_offset & (ELF_PAGE_SIZE - 1)))
            return ELF64_INVALID;
        if (alignment > 1 && (!power_of_two(alignment) ||
            ((virtual_address - file_offset) & (alignment - 1)))) return ELF64_INVALID;

        uint64_t memory_end = virtual_address + memory_size;
        uint64_t page_start = page_down(virtual_address);
        uint64_t page_end = (memory_end + ELF_PAGE_SIZE - 1) & ~(ELF_PAGE_SIZE - 1);
        if (page_end < memory_end || page_end <= page_start) return ELF64_INVALID;
        uint64_t segment_pages64 = (page_end - page_start) / ELF_PAGE_SIZE;
        if (segment_pages64 > ELF64_MAX_IMAGE_PAGES - result.page_count)
            return ELF64_UNSUPPORTED;
        for (uint32_t j = 0; j < result.segment_count; ++j) {
            const elf64_segment_t *prior = &result.segments[j];
            uint64_t prior_start = page_down(prior->virtual_address);
            uint64_t prior_end = prior_start + (uint64_t)prior->page_count * ELF_PAGE_SIZE;
            if (page_start < prior_end && prior_start < page_end) return ELF64_UNSUPPORTED;
        }

        elf64_segment_t *segment = &result.segments[result.segment_count++];
        segment->virtual_address = virtual_address;
        segment->memory_size = memory_size;
        segment->file_offset = file_offset;
        segment->file_size = file_size;
        segment->flags = flags;
        segment->first_page = result.page_count;
        segment->page_count = (uint32_t)segment_pages64;
        result.page_count += segment->page_count;
        if ((flags & ELF_PF_X) && entry >= virtual_address &&
            entry - virtual_address < file_size) entry_is_executable = 1;
    }
    if (!result.segment_count || !result.page_count || !entry_is_executable)
        return ELF64_INVALID;
    *plan = result;
    return ELF64_OK;
}

static int ops_valid(const elf64_memory_ops_t *ops) {
    return ops && ops->page_is_unmapped && ops->page_allocate && ops->page_release &&
           ops->map_page && ops->unmap_page;
}

static elf64_status_t rollback(const elf64_memory_ops_t *ops, elf64_loaded_image_t *loaded) {
    int failed = 0;
    while (loaded->page_count) {
        elf64_page_t page = loaded->pages[loaded->page_count - 1];
        if (!ops->unmap_page(ops->context, page.virtual_address)) {
            failed = 1;
            break;
        }
        --loaded->page_count;
        if (!ops->page_release(ops->context, page.physical_address)) failed = 1;
    }
    if (failed) return ELF64_ROLLBACK_FAILED;
    loaded->entry = 0;
    return ELF64_OK;
}

elf64_status_t elf64_load(const void *image_pointer, size_t image_size, uint8_t levels,
                          uint8_t nx_enabled, const elf64_memory_ops_t *ops,
                          elf64_loaded_image_t *loaded) {
    if (!ops_valid(ops) || !loaded) return ELF64_INVALID;
    loaded->entry = 0;
    loaded->page_count = 0;
    if (!nx_enabled) return ELF64_UNSUPPORTED;

    elf64_plan_t plan;
    elf64_status_t status = elf64_parse(image_pointer, image_size, levels, &plan);
    if (status != ELF64_OK) return status;
    const uint8_t *image = (const uint8_t *)image_pointer;

    /* Preflight every page before allocating frames or changing the address space. */
    for (uint32_t s = 0; s < plan.segment_count; ++s) {
        const elf64_segment_t *segment = &plan.segments[s];
        uint64_t first = page_down(segment->virtual_address);
        for (uint32_t p = 0; p < segment->page_count; ++p) {
            if (ops->page_is_unmapped(ops->context, first + (uint64_t)p * ELF_PAGE_SIZE) != 1)
                return ELF64_MAPPING_FAILED;
        }
    }

    for (uint32_t s = 0; s < plan.segment_count; ++s) {
        const elf64_segment_t *segment = &plan.segments[s];
        uint64_t first_page = page_down(segment->virtual_address);
        uint64_t segment_end = segment->virtual_address + segment->memory_size;
        uint64_t file_end = segment->virtual_address + segment->file_size;
        uint32_t map_flags = VM_MAP_USER;
        if (segment->flags & ELF_PF_W) map_flags |= VM_MAP_WRITE;
        if (segment->flags & ELF_PF_X) map_flags |= VM_MAP_EXEC;

        for (uint32_t p = 0; p < segment->page_count; ++p) {
            uint64_t virtual_page = first_page + (uint64_t)p * ELF_PAGE_SIZE;
            uint64_t physical_page = 0;
            uint8_t *writable = 0;
            int allocated = ops->page_allocate(ops->context, &physical_page, &writable);
            if (!allocated || !writable || !physical_page ||
                (physical_page & (ELF_PAGE_SIZE - 1)) ||
                (physical_page & ~vm_physical_mask(52))) {
                int released = !allocated || !physical_page ||
                              ops->page_release(ops->context, physical_page);
                status = released ? ELF64_NO_MEMORY : ELF64_ROLLBACK_FAILED;
                goto fail;
            }
            for (uint32_t b = 0; b < ELF_PAGE_SIZE; ++b) writable[b] = 0;

            uint64_t copy_start = virtual_page > segment->virtual_address
                                      ? virtual_page : segment->virtual_address;
            uint64_t page_end = virtual_page + ELF_PAGE_SIZE;
            uint64_t copy_end = page_end < file_end ? page_end : file_end;
            if (copy_start < copy_end) {
                uint64_t source = segment->file_offset + (copy_start - segment->virtual_address);
                uint64_t destination = copy_start - virtual_page;
                for (uint64_t b = 0; b < copy_end - copy_start; ++b)
                    writable[destination + b] = image[source + b];
            }
            if (virtual_page >= segment_end) {
                (void)ops->page_release(ops->context, physical_page);
                status = ELF64_INVALID;
                goto fail;
            }
            if (!ops->map_page(ops->context, virtual_page, physical_page, map_flags)) {
                status = ops->page_release(ops->context, physical_page)
                              ? ELF64_MAPPING_FAILED : ELF64_ROLLBACK_FAILED;
                goto fail;
            }
            loaded->pages[loaded->page_count].virtual_address = virtual_page;
            loaded->pages[loaded->page_count].physical_address = physical_page;
            ++loaded->page_count;
        }
    }
    loaded->entry = plan.entry;
    return ELF64_OK;

fail:
    if (rollback(ops, loaded) != ELF64_OK) return ELF64_ROLLBACK_FAILED;
    return status;
}

elf64_status_t elf64_unload(const elf64_memory_ops_t *ops, elf64_loaded_image_t *loaded) {
    if (!ops_valid(ops) || !loaded || loaded->page_count > ELF64_MAX_IMAGE_PAGES)
        return ELF64_INVALID;
    elf64_status_t status = rollback(ops, loaded);
    if (status == ELF64_OK) loaded->entry = 0;
    return status;
}








