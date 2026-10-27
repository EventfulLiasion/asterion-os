#ifndef ASTERION_KERNEL_VM_CORE_H
#define ASTERION_KERNEL_VM_CORE_H

#include <stdint.h>

#define VM_PAGE_SIZE UINT64_C(4096)
#define VM_MAX_OWNED_MAPPINGS 256u
#define VM_MAX_OWNED_TABLES 4096u

enum {
    VM_MAP_WRITE = 1u << 0,
    VM_MAP_EXEC  = 1u << 1,
    VM_MAP_USER  = 1u << 2,
    VM_MAP_PWT   = 1u << 3,
    VM_MAP_PCD   = 1u << 4,
    VM_MAP_GLOBAL = 1u << 5
};

enum {
    VM_OK = 0,
    VM_INVALID = -1,
    VM_CONFLICT = -2,
    VM_NO_MEMORY = -3,
    VM_CORRUPT_TABLE = -4,
    VM_NOT_OWNED = -5,
    VM_CAPACITY = -6,
    VM_NOT_MAPPED = -7
};

typedef uint64_t *(*vm_resolve_table_fn)(void *context, uint64_t physical);
/* Successful allocations must return an aligned, zero-filled table page. */
typedef int (*vm_allocate_table_fn)(void *context, uint64_t *physical, uint64_t **table);
typedef void (*vm_release_table_fn)(void *context, uint64_t physical);

typedef struct {
    uint64_t *root;
    uint64_t root_physical;
    uint8_t levels;
    uint8_t physical_bits;
    uint8_t nx_enabled;
    uint8_t reserved;
    void *context;
    vm_resolve_table_fn resolve;
    vm_allocate_table_fn allocate;
    vm_release_table_fn release;
    uint64_t owned_virtual_pages[VM_MAX_OWNED_MAPPINGS];
    uint32_t owned_mapping_count;
    uint64_t owned_table_physical[VM_MAX_OWNED_TABLES];
    uint32_t owned_table_count;
} vm_space_t;

int vm_is_canonical(uint64_t virtual_address, uint8_t levels);
uint64_t vm_physical_mask(uint8_t physical_bits);
int vm_encode_leaf(uint64_t physical, uint32_t flags, uint8_t physical_bits,
                   uint8_t nx_enabled, uint64_t *entry);
int vm_core_map(vm_space_t *space, uint64_t virtual_address,
                uint64_t physical_address, uint32_t flags);
int vm_core_unmap(vm_space_t *space, uint64_t virtual_address);
int vm_core_translate(const vm_space_t *space, uint64_t virtual_address,
                      uint64_t *physical_address, uint32_t *flags);
/* Replace cache bits on one 4 KiB mapping, demoting huge leaves without changing neighbors. */
int vm_core_set_page_cache_flags(vm_space_t *space, uint64_t virtual_address, uint32_t cache_flags);
int vm_core_find_physical_alias(const vm_space_t *space, uint64_t physical_address,
                                uint64_t *virtual_address, uint32_t *flags);
int vm_user_address_valid(uint64_t virtual_address, uint64_t size, uint8_t levels);
int vm_core_space_init(vm_space_t *space, uint64_t *root, uint64_t root_physical,
                       uint8_t levels, uint8_t physical_bits, uint8_t nx_enabled,
                       void *context, vm_resolve_table_fn resolve,
                       vm_allocate_table_fn allocate, vm_release_table_fn release);

#endif
