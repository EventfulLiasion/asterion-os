#include <stdint.h>
#include "elf64_loader.h"
#include "vm.h"
#include "pmm.h"

static int kernel_page_is_unmapped(void *context, uint64_t va) {
    vm_space_t *space = (vm_space_t *)context;
    uint64_t pa = 0;
    int result = vm_space_translate(space, va, &pa, 0);
    return result == VM_NOT_MAPPED ? 1 : (result == VM_OK ? 0 : -1);
}

static int kernel_page_allocate(void *context, uint64_t *physical, uint8_t **writable) {
    (void)context;
    if (!physical || !writable || !pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, physical)) return 0;
    uint64_t translated = 0;
    uint32_t flags = 0;
    if (vm_translate_address(*physical, &translated, &flags) != VM_OK ||
        translated != *physical || !(flags & VM_MAP_WRITE)) {
        (void)pmm_free(*physical, 1);
        *physical = 0;
        return 0;
    }
    *writable = (uint8_t *)(uintptr_t)*physical;
    return 1;
}

static int kernel_page_release(void *context, uint64_t physical) {
    (void)context;
    return pmm_free(physical, 1);
}

static int kernel_map_page(void *context, uint64_t va, uint64_t pa, uint32_t flags) {
    return vm_space_map_page((vm_space_t *)context, va, pa, flags) == VM_OK;
}

static int kernel_unmap_page(void *context, uint64_t va) {
    return vm_space_unmap_page((vm_space_t *)context, va) == VM_OK;
}

static elf64_memory_ops_t kernel_ops(vm_space_t *space) {
    elf64_memory_ops_t ops = {
        .context = space,
        .page_is_unmapped = kernel_page_is_unmapped,
        .page_allocate = kernel_page_allocate,
        .page_release = kernel_page_release,
        .map_page = kernel_map_page,
        .unmap_page = kernel_unmap_page
    };
    return ops;
}

elf64_status_t elf64_load_user_image(vm_space_t *space, const void *image,
                                     size_t image_size, elf64_loaded_image_t *loaded) {
    if (!space || !space->root || !loaded || !vm_space_audit_user_mappings(space, 0, 0))
        return ELF64_INVALID;
    elf64_plan_t plan;
    elf64_status_t status = elf64_parse(image, image_size, space->levels, &plan);
    if (status != ELF64_OK) return status;

    uint64_t user_start = UINT64_MAX, user_end = 0;
    for (uint32_t i = 0; i < plan.segment_count; ++i) {
        const elf64_segment_t *segment = &plan.segments[i];
        uint64_t start = segment->virtual_address & ~(VM_PAGE_SIZE - 1);
        uint64_t end = start + (uint64_t)segment->page_count * VM_PAGE_SIZE;
        if (start < user_start) user_start = start;
        if (end > user_end) user_end = end;
    }

    elf64_memory_ops_t ops = kernel_ops(space);
    status = elf64_load(image, image_size, space->levels, space->nx_enabled, &ops, loaded);
    if (status != ELF64_OK) return status;
    if (!vm_space_audit_user_mappings(space, user_start, user_end)) {
        elf64_status_t cleanup = elf64_unload(&ops, loaded);
        return cleanup == ELF64_OK ? ELF64_MAPPING_FAILED : ELF64_ROLLBACK_FAILED;
    }
    return ELF64_OK;
}

elf64_status_t elf64_unload_user_image(vm_space_t *space, elf64_loaded_image_t *loaded) {
    if (!space || !space->root || !loaded) return ELF64_INVALID;
    elf64_memory_ops_t ops = kernel_ops(space);
    return elf64_unload(&ops, loaded);
}

