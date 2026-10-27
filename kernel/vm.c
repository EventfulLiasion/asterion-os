#include <stddef.h>
#include <stdint.h>
#include "vm.h"
#include "pmm.h"
#include "../arch/x86_64/interrupts.h"
#include "../drivers/serial.h"
#include "../include/boot_info.h"

#define CR0_PG (UINT64_C(1) << 31)
#define CR0_WP (UINT64_C(1) << 16)
#define CR4_LA57 (UINT64_C(1) << 12)
#define EFER_MSR UINT32_C(0xC0000080)
#define EFER_LMA (UINT64_C(1) << 10)
#define EFER_NXE (UINT64_C(1) << 11)
#define IA32_PAT_MSR UINT32_C(0x00000277)
#define CPUID_PAT (1u << 16)
#define VM_MAX_PAGE_TABLES 8192u
#define VM_RESOLVE_CACHE 8192u
#define TEST_VIRTUAL_BASE UINT64_C(0x0000400000000000)

static vm_space_t firmware_space;
static vm_space_t kernel_space;
static uint64_t allocated_tables[VM_MAX_PAGE_TABLES];
static uint32_t allocated_table_count;
static uint64_t resolved_tables[VM_RESOLVE_CACHE];
static uint32_t resolved_table_count;
static uint64_t physical_mask;
static uint64_t old_cr3;
static uint8_t active_level_count;
static uint8_t hardware_nx;

static uint64_t *resolve_table(void *context, uint64_t physical);
static int table_allocate(void *context, uint64_t *physical, uint64_t **table);
static void table_release(void *context, uint64_t physical);

static void space_initialize(vm_space_t *space, uint64_t *root, uint64_t root_physical,
                             uint8_t levels, uint8_t physical_bits, uint8_t nx_enabled) {
    (void)vm_core_space_init(space, root, root_physical, levels, physical_bits,
                             nx_enabled, space, resolve_table,
                             table_allocate, table_release);
}

static uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0,%0":"=r"(v)); return v; }
static uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3,%0":"=r"(v)); return v; }
static uint64_t read_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4,%0":"=r"(v)); return v; }
static uint64_t read_efer(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(EFER_MSR));
    return ((uint64_t)hi << 32) | lo;
}
static void write_efer(uint64_t value) {
    uint32_t lo = (uint32_t)value, hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr"::"a"(lo),"d"(hi),"c"(EFER_MSR):"memory");
}
static void write_cr3(uint64_t value) { __asm__ volatile("mov %0,%%cr3"::"r"(value):"memory"); }
static void invalidate_page(uint64_t address) { __asm__ volatile("invlpg (%0)"::"r"((uintptr_t)address):"memory"); }
static uintptr_t read_rsp(void) { uintptr_t value; __asm__ volatile("mov %%rsp,%0":"=r"(value)); return value; }

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid":"=a"(*a),"=b"(*b),"=c"(*c),"=d"(*d):"a"(leaf),"c"(subleaf));
}

static int uncached_pat_entries_available(void){
    uint32_t a,b,c,d;cpuid(1,0,&a,&b,&c,&d);
    if(!(d&CPUID_PAT))return 0;
    uint32_t low,high;
    __asm__ volatile("rdmsr":"=a"(low),"=d"(high):"c"(IA32_PAT_MSR));
    uint64_t pat=((uint64_t)high<<32)|low;
    /* PCD|PWT selects PAT[3] or PAT[7], depending on the PTE PAT bit. */
    return ((pat>>24)&0xFFu)==0 && ((pat>>56)&0xFFu)==0;
}
static void hex64(char output[19], uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    output[0] = '0'; output[1] = 'x';
    for (uint32_t i = 0; i < 16; ++i) output[2 + i] = digits[(value >> (60 - i * 4)) & 0xF];
    output[18] = 0;
}

static void log_register(const char *name, uint64_t value) {
    char text[72], hex[19];
    size_t i = 0;
    while (name[i] && i < sizeof(text) - 20) { text[i] = name[i]; ++i; }
    hex64(hex, value);
    for (uint32_t j = 0; j < 18 && i < sizeof(text) - 1; ++j) text[i++] = hex[j];
    text[i] = 0;
    serial_log(SERIAL_INFO, text);
}

static void log_translation(const char *name, uint64_t virtual_address,
                            uint64_t physical_address, uint32_t flags) {
    char text[112], va[19], pa[19], fp[19];
    size_t i = 0;
    while (name[i] && i < 24) { text[i] = name[i]; ++i; }
    text[i++] = ' ';
    hex64(va, virtual_address); hex64(pa, physical_address); hex64(fp, flags);
    for (uint32_t j = 0; j < 18 && i < sizeof(text) - 1; ++j) text[i++] = va[j];
    if (i < sizeof(text) - 1) text[i++] = '-';
    if (i < sizeof(text) - 1) text[i++] = '>';
    for (uint32_t j = 0; j < 18 && i < sizeof(text) - 1; ++j) text[i++] = pa[j];
    if (i < sizeof(text) - 1) text[i++] = ' ';
    for (uint32_t j = 0; j < 18 && i < sizeof(text) - 1; ++j) text[i++] = fp[j];
    text[i] = 0;
    serial_log(SERIAL_INFO, text);
}

static int cache_contains(uint64_t physical) {
    for (uint32_t i = 0; i < resolved_table_count; ++i)
        if (resolved_tables[i] == physical) return 1;
    return 0;
}

static int cache_add(uint64_t physical) {
    if (cache_contains(physical)) return 1;
    if (resolved_table_count == VM_RESOLVE_CACHE) return 0;
    resolved_tables[resolved_table_count++] = physical;
    return 1;
}

static uint64_t *resolve_table(void *context, uint64_t physical) {
    (void)context;
    if ((physical & (VM_PAGE_SIZE - 1)) || (physical & ~physical_mask) ||
        !vm_is_canonical(physical, active_level_count)) return 0;
    if (cache_contains(physical)) return (uint64_t *)(uintptr_t)physical;
    uint64_t probe = 0;
    if (!x86_probe_read64((uintptr_t)physical, &probe) ||
        !x86_probe_read64((uintptr_t)(physical + VM_PAGE_SIZE - sizeof(uint64_t)), &probe) ||
        !cache_add(physical)) return 0;
    return (uint64_t *)(uintptr_t)physical;
}

static int table_allocate(void *context, uint64_t *physical, uint64_t **table) {
    vm_space_t *owner = (vm_space_t *)context;
    uint64_t address = 0;
    if (!physical || !table || allocated_table_count == VM_MAX_PAGE_TABLES ||
        (owner && owner->owned_table_count == VM_MAX_OWNED_TABLES) ||
        !pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, &address)) return 0;
    uint64_t translated = 0;
    uint32_t flags = 0;
    if (vm_core_translate(&firmware_space, address, &translated, &flags) != VM_OK ||
        translated != address || !(flags & VM_MAP_WRITE) ||
        !vm_is_canonical(address, active_level_count)) {
        (void)pmm_free(address, 1);
        return 0;
    }
    uint64_t *memory = (uint64_t *)(uintptr_t)address;
    for (uint32_t i = 0; i < 512; ++i) memory[i] = 0;
    if (!cache_add(address)) {
        (void)pmm_free(address, 1);
        return 0;
    }
    allocated_tables[allocated_table_count++] = address;
    if (owner) owner->owned_table_physical[owner->owned_table_count++] = address;
    *physical = address;
    *table = memory;
    return 1;
}

static void table_release(void *context, uint64_t physical) {
    vm_space_t *owner = (vm_space_t *)context;
    for (uint32_t i = 0; i < allocated_table_count; ++i) {
        if (allocated_tables[i] == physical) {
            allocated_tables[i] = allocated_tables[--allocated_table_count];
            if (owner) {
                for (uint32_t j = 0; j < owner->owned_table_count; ++j) {
                    if (owner->owned_table_physical[j] == physical) {
                        owner->owned_table_physical[j] = owner->owned_table_physical[--owner->owned_table_count];
                        break;
                    }
                }
            }
            (void)pmm_free(physical, 1);
            for (uint32_t j = 0; j < resolved_table_count; ++j) {
                if (resolved_tables[j] == physical) {
                    resolved_tables[j] = resolved_tables[--resolved_table_count];
                    break;
                }
            }
            return;
        }
    }
}

static int clone_table_tree(uint64_t source_physical, uint8_t level,
                            uint64_t *destination_physical, vm_space_t *owner) {
    uint64_t *source = resolve_table(0, source_physical);
    uint64_t *destination = 0;
    if (!source || !table_allocate(owner, destination_physical, &destination)) return 0;
    for (uint32_t i = 0; i < 512; ++i) destination[i] = source[i];
    if (level == 1) return 1;

    for (uint32_t i = 0; i < 512; ++i) {
        uint64_t entry = source[i];
        if (!(entry & 1u)) continue;
        if ((level == 3 || level == 2) && (entry & (UINT64_C(1) << 7))) continue;
        uint64_t child = entry & physical_mask;
        uint64_t cloned_child = 0;
        if (!clone_table_tree(child, (uint8_t)(level - 1), &cloned_child, owner)) return 0;
        destination[i] = (entry & ~physical_mask) | cloned_child;
    }
    return 1;
}

static void release_space_tables(vm_space_t *space) {
    while (space && space->owned_table_count)
        table_release(space, space->owned_table_physical[space->owned_table_count - 1]);
}

static void audit_one(const char *name, uint64_t virtual_address,
                      uint64_t *saved_physical, uint32_t *saved_flags) {
    uint64_t physical = 0;
    uint32_t flags = 0;
    if (vm_core_translate(&firmware_space, virtual_address, &physical, &flags) != VM_OK) {
        serial_log(SERIAL_ERROR, "VM AUDIT FAILED: required address is not mapped by UEFI.");
        *saved_physical = UINT64_MAX;
        *saved_flags = 0;
        return;
    }
    log_translation(name, virtual_address, physical, flags);
    *saved_physical = physical;
    *saved_flags = flags;
}

static int audit_preserved(const char *name, uint64_t virtual_address, uint64_t expected) {
    uint64_t physical = 0;
    uint32_t flags = 0;
    if (vm_core_translate(&kernel_space, virtual_address, &physical, &flags) != VM_OK ||
        physical != expected) {
        serial_log(SERIAL_ERROR, name);
        return 0;
    }
    log_translation("VM preserved", virtual_address, physical, flags);
    return 1;
}

int vm_init(const void *boot_info_pointer) {
    const boot_info_t *info = (const boot_info_t *)boot_info_pointer;
    uint32_t a, b, c, d;
    cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a < 0x80000008u) {
        serial_log(SERIAL_ERROR, "VM init failed: CPU physical-address width is unavailable.");
        return 0;
    }
    cpuid(0x80000008u, 0, &a, &b, &c, &d);
    uint8_t physical_bits = (uint8_t)(a & 0xFF);
    if (physical_bits < 36 || physical_bits > 52) {
        serial_log(SERIAL_ERROR, "VM init failed: unsupported physical-address width.");
        return 0;
    }
    physical_mask = vm_physical_mask(physical_bits);

    uint64_t cr0 = read_cr0();
    uint64_t cr4 = read_cr4();
    uint64_t efer = read_efer();
    old_cr3 = read_cr3();
    active_level_count = (cr4 & CR4_LA57) ? 5 : 4;
    hardware_nx = info && (info->cpu_extended_feature_edx & (1u << 20)) != 0;
    log_register("VM inherited CR0=", cr0);
    log_register("VM inherited CR3=", old_cr3);
    log_register("VM inherited CR4=", cr4);
    log_register("VM inherited EFER=", efer);
    if (!(cr0 & CR0_PG) || !(efer & EFER_LMA) || !(cr4 & (UINT64_C(1) << 5))) {
        serial_log(SERIAL_ERROR, "VM init failed: paging/PAE/long mode is not active.");
        return 0;
    }
    if ((cr4 & CR4_LA57) && active_level_count != 5) return 0;
    if (!hardware_nx) serial_log(SERIAL_WARN, "VM NX unavailable; non-executable data mappings cannot be enforced.");

    uint64_t root_physical = old_cr3 & physical_mask;
    if (!vm_is_canonical(root_physical, active_level_count)) {
        serial_log(SERIAL_ERROR, "VM init failed: inherited CR3 root is not identity-addressable.");
        return 0;
    }
    uint64_t root_probe = 0;
    if (!x86_probe_read64((uintptr_t)root_physical, &root_probe) ||
        !x86_probe_read64((uintptr_t)(root_physical + VM_PAGE_SIZE - sizeof(uint64_t)), &root_probe)) {
        serial_log(SERIAL_ERROR, "VM init failed: inherited CR3 page is not readable at its physical address.");
        return 0;
    }
    if (!cache_add(root_physical)) return 0;

    space_initialize(&firmware_space, (uint64_t *)(uintptr_t)root_physical,
                     root_physical, active_level_count, physical_bits,
                     (uint8_t)(hardware_nx || (efer & EFER_NXE)));

    uint64_t critical_va[6], critical_pa[6];
    uint32_t critical_flags[6];
    critical_va[0] = (uint64_t)(uintptr_t)&vm_init;
    critical_va[1] = (uint64_t)read_rsp();
    critical_va[2] = (uint64_t)(uintptr_t)info;
    critical_va[3] = (uint64_t)(uintptr_t)info->memory_map;
    critical_va[4] = info->framebuffer_base;
    critical_va[5] = root_physical;
    const char *const names[6] = {"VM code", "VM active RSP", "VM boot handoff",
                                  "VM memory map", "VM framebuffer", "VM old CR3 page"};
    serial_log(SERIAL_INFO, "VM audit: inherited UEFI mappings (VA -> PA, effective W/U/X flags).");
    for (uint32_t i = 0; i < 6; ++i) {
        audit_one(names[i], critical_va[i], &critical_pa[i], &critical_flags[i]);
        if (critical_pa[i] == UINT64_MAX) return 0;
    }
    if (critical_pa[4] != info->framebuffer_base || critical_pa[3] != (uintptr_t)info->memory_map) {
        serial_log(SERIAL_ERROR, "VM audit failed: framebuffer or UEFI map is not identity mapped.");
        return 0;
    }
    if (!(critical_flags[0] & VM_MAP_EXEC) || !(critical_flags[1] & VM_MAP_WRITE) ||
        !(critical_flags[2] & VM_MAP_WRITE) || !(critical_flags[4] & VM_MAP_WRITE)) {
        serial_log(SERIAL_ERROR, "VM audit failed: kernel code, stack, handoff, or framebuffer permissions are incompatible.");
        return 0;
    }

    space_initialize(&kernel_space, (uint64_t *)(uintptr_t)root_physical, root_physical,
                     active_level_count, physical_bits,
                     (uint8_t)(hardware_nx && (efer & EFER_NXE)));
    uint64_t new_root = 0;
    if (!clone_table_tree(root_physical, active_level_count, &new_root, &kernel_space)) {
        release_space_tables(&kernel_space);
        serial_log(SERIAL_ERROR, "VM init failed: could not clone every inherited page-table mapping.");
        return 0;
    }
    kernel_space.root_physical = new_root;
    kernel_space.root = (uint64_t *)(uintptr_t)new_root;

    if (hardware_nx && !(efer & EFER_NXE)) {
        write_efer(efer | EFER_NXE);
        efer |= EFER_NXE;
    }
    uint64_t protected_cr0 = cr0 | CR0_WP;
    __asm__ volatile("mov %0,%%cr0"::"r"(protected_cr0):"memory");

    uint64_t flags_before = 0;
    __asm__ volatile("pushfq; popq %0; cli":"=r"(flags_before)::"memory");
    /* Start every kernel/process root in PCID 0; CR3 writes flush its non-global TLB entries. */
    write_cr3(new_root);
    int preserved = 1;
    for (uint32_t i = 0; i < 6; ++i)
        if (!audit_preserved(names[i], critical_va[i], critical_pa[i])) preserved = 0;
    if (!preserved) {
        write_cr3(old_cr3);
        __asm__ volatile("mov %0,%%cr0"::"r"(cr0):"memory");
        release_space_tables(&kernel_space);
        if (flags_before & (UINT64_C(1) << 9)) __asm__ volatile("sti" ::: "memory");
        serial_log(SERIAL_ERROR, "VM init rolled back; UEFI page tables remain active.");
        return 0;
    }
    if (flags_before & (UINT64_C(1) << 9)) __asm__ volatile("sti" ::: "memory");

    active_level_count = kernel_space.levels;
    log_register("VM kernel CR3=", read_cr3());
    serial_log(SERIAL_INFO, "VM root switched: complete inherited tree cloned into PMM-owned tables.");
    return 1;
}

int vm_address_space_create(vm_space_t *space) {
    if (!space || !kernel_space.root || space == &kernel_space || space == &firmware_space ||
        !vm_space_audit_user_mappings(&kernel_space, 0, 0)) return 0;
    space_initialize(space, kernel_space.root, kernel_space.root_physical,
                     kernel_space.levels,
                     kernel_space.physical_bits, kernel_space.nx_enabled);
    uint64_t root = 0;
    if (!clone_table_tree(kernel_space.root_physical, kernel_space.levels, &root, space)) {
        release_space_tables(space);
        return 0;
    }
    space->root_physical = root;
    space->root = (uint64_t *)(uintptr_t)root;
    return 1;
}

void vm_address_space_destroy(vm_space_t *space) {
    if (!space || space == &kernel_space || space == &firmware_space) return;
    release_space_tables(space);
    space->root = 0;
    space->root_physical = 0;
    space->owned_mapping_count = 0;
}

int vm_space_map_page(vm_space_t *space, uint64_t va, uint64_t pa, uint32_t flags) {
    if (!space || !space->root) return VM_INVALID;
    int result = vm_core_map(space, va, pa, flags);
    if (result == VM_OK && (read_cr3() & physical_mask) == space->root_physical)
        invalidate_page(va);
    return result;
}

int vm_space_unmap_page(vm_space_t *space, uint64_t va) {
    if (!space || !space->root) return VM_INVALID;
    int result = vm_core_unmap(space, va);
    if (result == VM_OK && (read_cr3() & physical_mask) == space->root_physical)
        invalidate_page(va);
    return result;
}

int vm_space_translate(const vm_space_t *space, uint64_t va, uint64_t *pa, uint32_t *flags) {
    if (!space || !space->root) return VM_INVALID;
    return vm_core_translate(space, va, pa, flags);
}

int vm_space_activate(vm_space_t *space) {
    if (!space || !space->root || space->levels != active_level_count) return 0;
    /* Reuse PCID 0 for every root so CR3 writes invalidate that root's non-global TLB state. */
    write_cr3(space->root_physical);
    return (read_cr3() & physical_mask) == space->root_physical;
}

int vm_activate_kernel_space(void) {
    return vm_space_activate(&kernel_space);
}

static int audit_user_tree(const vm_space_t *space, uint64_t physical, uint8_t level,
                           uint64_t prefix, int ancestors_user,
                           uint64_t user_start, uint64_t user_end) {
    uint64_t *table = space->resolve(space->context, physical);
    if (!table) return 0;
    for (uint32_t i = 0; i < 512; ++i) {
        uint64_t entry = table[i];
        if (!(entry & 1u)) continue;
        int effective_user = ancestors_user && ((entry & 4u) != 0);
        uint64_t address = prefix | ((uint64_t)i << (12 + (uint32_t)(level - 1) * 9));
        int huge = (level == 3 || level == 2) && (entry & (UINT64_C(1) << 7));
        if (level == 1 || huge) {
            if (!effective_user) continue;
            uint64_t span = UINT64_C(1) << (huge ? (level == 3 ? 30 : 21) : 12);
            if (address < user_start || address > UINT64_MAX - span || address + span > user_end)
                return 0;
            continue;
        }
        if (!audit_user_tree(space, entry & physical_mask, (uint8_t)(level - 1),
                             address, effective_user, user_start, user_end)) return 0;
    }
    return 1;
}

int vm_space_audit_user_mappings(const vm_space_t *space, uint64_t user_start, uint64_t user_end) {
    if (!space || !space->root || (user_start > user_end) ||
        ((user_start || user_end) && !vm_user_address_valid(user_start, user_end - user_start,
                                                             space->levels))) return 0;
    return audit_user_tree(space, space->root_physical, space->levels, 0, 1,
                           user_start, user_end);
}

int vm_kernel_mapping_is_supervisor(uint64_t virtual_address) {
    uint64_t physical = 0;
    uint32_t flags = 0;
    return kernel_space.root &&
           vm_core_translate(&kernel_space, virtual_address, &physical, &flags) == VM_OK &&
           !(flags & VM_MAP_USER);
}

int vm_map_page(uint64_t virtual_address, uint64_t physical_address, uint32_t flags) {
    if ((flags & (VM_MAP_PCD|VM_MAP_PWT)) == (VM_MAP_PCD|VM_MAP_PWT) &&
        !uncached_pat_entries_available()) return VM_INVALID;
    if (!kernel_space.root) return VM_INVALID;
    int result = vm_core_map(&kernel_space, virtual_address, physical_address, flags);
    if (result == VM_OK) invalidate_page(virtual_address);
    return result;
}

int vm_set_page_cache_flags(uint64_t virtual_address,uint32_t cache_flags){
    if ((cache_flags & (VM_MAP_PCD|VM_MAP_PWT)) == (VM_MAP_PCD|VM_MAP_PWT) &&
        !uncached_pat_entries_available()) return VM_INVALID;
    if(!kernel_space.root)return VM_INVALID;
    /* These page tables are active; invlpg must invalidate the mapping being changed. */
    if((read_cr3()&physical_mask)!=(kernel_space.root_physical&physical_mask))return VM_INVALID;
    int result=vm_core_set_page_cache_flags(&kernel_space,virtual_address,cache_flags);
    if(result==VM_OK)invalidate_page(virtual_address);
    return result;
}
int vm_unmap_page(uint64_t virtual_address) {
    if (!kernel_space.root) return VM_INVALID;
    int result = vm_core_unmap(&kernel_space, virtual_address);
    if (result == VM_OK) invalidate_page(virtual_address);
    return result;
}

int vm_find_physical_alias(uint64_t physical_address,uint64_t *virtual_address,uint32_t *flags){
    if(!kernel_space.root)return VM_INVALID;
    return vm_core_find_physical_alias(&kernel_space,physical_address,virtual_address,flags);
}

int vm_translate_address(uint64_t virtual_address, uint64_t *physical_address,
                         uint32_t *flags) {
    if (!kernel_space.root) return VM_INVALID;
    return vm_core_translate(&kernel_space, virtual_address, physical_address, flags);
}

int vm_self_test(void) {
    uint64_t test_va = TEST_VIRTUAL_BASE;
    uint64_t ignored = 0;
    uint32_t ignored_flags = 0;
    uint32_t tries = 0;
    while (tries < 1024) {
        int status = vm_translate_address(test_va, &ignored, &ignored_flags);
        if (status == VM_NOT_MAPPED) break;
        if (status != VM_OK || !vm_is_canonical(test_va, active_level_count)) return 0;
        test_va += VM_PAGE_SIZE;
        ++tries;
    }
    if (tries == 1024) {
        serial_log(SERIAL_ERROR, "VM self-test failed: no isolated unmapped test page found.");
        return 0;
    }

    uint64_t physical = 0;
    if (!pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, &physical)) return 0;
    if (vm_map_page(test_va, physical, VM_MAP_WRITE) != VM_OK) {
        (void)pmm_free(physical, 1);
        return 0;
    }
    uint64_t translated = 0;
    uint32_t flags = 0;
    if (vm_translate_address(test_va, &translated, &flags) != VM_OK ||
        translated != physical || !(flags & VM_MAP_WRITE) || (flags & VM_MAP_EXEC)) {
        (void)vm_unmap_page(test_va);
        (void)pmm_free(physical, 1);
        return 0;
    }
    volatile uint64_t *memory = (volatile uint64_t *)(uintptr_t)test_va;
    *memory = UINT64_C(0x4153544552494F4E);
    if (*memory != UINT64_C(0x4153544552494F4E) || vm_unmap_page(test_va) != VM_OK) {
        (void)pmm_free(physical, 1);
        return 0;
    }
    uint64_t error = 0, cr2 = 0;
    if (!x86_expect_page_fault((uintptr_t)test_va, &error, &cr2) || cr2 != test_va ||
        (error & 1u) || !pmm_free(physical, 1)) return 0;
    serial_log(SERIAL_INFO, "SELFTEST PASS: VM map/translate/write/unmap and isolated not-present page fault.");
    log_register("VM test #PF error=", error);
    log_register("VM test CR2=", cr2);

    if (hardware_nx) {
        uint64_t executable_test_page = 0;
        if (!pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, &executable_test_page) ||
            vm_map_page(test_va, executable_test_page, VM_MAP_WRITE) != VM_OK) {
            if (executable_test_page) (void)pmm_free(executable_test_page, 1);
            return 0;
        }
        volatile uint8_t *code = (volatile uint8_t *)(uintptr_t)test_va;
        code[0] = 0xC3; /* RET: execution is harmless if NX enforcement is broken. */
        if (!x86_expect_execute_fault((uintptr_t)test_va, &error, &cr2) || cr2 != test_va ||
            vm_unmap_page(test_va) != VM_OK || !pmm_free(executable_test_page, 1)) return 0;
        serial_log(SERIAL_INFO, "SELFTEST PASS: NX blocked instruction fetch from a writable data mapping.");
        log_register("VM NX #PF error=", error);
    }
    return 1;
}

uint8_t vm_active_levels(void) { return active_level_count; }
uint8_t vm_nx_enabled(void) { return kernel_space.nx_enabled; }
