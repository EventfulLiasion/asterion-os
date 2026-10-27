#ifndef ASTERION_KERNEL_VM_H
#define ASTERION_KERNEL_VM_H

#include <stdint.h>
#include "vm_core.h"

int vm_init(const void *boot_info);
int vm_address_space_create(vm_space_t *space);
void vm_address_space_destroy(vm_space_t *space);
int vm_space_map_page(vm_space_t *space, uint64_t virtual_address,
                      uint64_t physical_address, uint32_t flags);
int vm_space_unmap_page(vm_space_t *space, uint64_t virtual_address);
int vm_space_translate(const vm_space_t *space, uint64_t virtual_address,
                       uint64_t *physical_address, uint32_t *flags);
int vm_space_activate(vm_space_t *space);
int vm_activate_kernel_space(void);
int vm_space_audit_user_mappings(const vm_space_t *space,
                                 uint64_t user_start, uint64_t user_end);
int vm_kernel_mapping_is_supervisor(uint64_t virtual_address);
int vm_map_page(uint64_t virtual_address, uint64_t physical_address, uint32_t flags);
int vm_unmap_page(uint64_t virtual_address);
int vm_translate_address(uint64_t virtual_address, uint64_t *physical_address,
                         uint32_t *flags);
int vm_set_page_cache_flags(uint64_t virtual_address, uint32_t cache_flags);
int vm_find_physical_alias(uint64_t physical_address, uint64_t *virtual_address,
                            uint32_t *flags);
int vm_self_test(void);
uint8_t vm_active_levels(void);
uint8_t vm_nx_enabled(void);

#endif
