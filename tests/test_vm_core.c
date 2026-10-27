#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../kernel/vm_core.h"

typedef struct {
    uint64_t tables[32][512];
    uint32_t allocated;
    uint32_t allocate_calls;
    uint32_t fail_on_call;
} test_tables_t;

static uint64_t table_physical(uint32_t index) { return UINT64_C(0x100000) + (uint64_t)index * VM_PAGE_SIZE; }

static uint64_t *resolve(void *context, uint64_t physical) {
    test_tables_t *state = context;
    if (physical < table_physical(0) || (physical - table_physical(0)) % VM_PAGE_SIZE) return NULL;
    uint64_t index = (physical - table_physical(0)) / VM_PAGE_SIZE;
    return index < 32 ? state->tables[index] : NULL;
}

static int allocate(void *context, uint64_t *physical, uint64_t **table) {
    test_tables_t *state = context;
    ++state->allocate_calls;
    if (state->fail_on_call && state->allocate_calls == state->fail_on_call) return 0;
    if (state->allocated >= 32) return 0;
    uint32_t index = state->allocated++;
    memset(state->tables[index], 0, sizeof(state->tables[index]));
    *physical = table_physical(index);
    *table = state->tables[index];
    return 1;
}

static void release(void *context, uint64_t physical) {
    test_tables_t *state = context;
    assert((physical - table_physical(0)) % VM_PAGE_SIZE == 0);
    uint32_t index = (uint32_t)((physical - table_physical(0)) / VM_PAGE_SIZE);
    assert(index > 0 && index < state->allocated);
    memset(state->tables[index], 0, sizeof(state->tables[index]));
    --state->allocated;
}

int main(void) {
    assert(vm_is_canonical(0x00007FFFFFFFFFFF, 4));
    assert(vm_is_canonical(0xFFFF800000000000, 4));
    assert(!vm_is_canonical(0x0000800000000000, 4));
    assert(vm_is_canonical(0x00FFFFFFFFFFFFFF, 5));
    assert(vm_is_canonical(0xFF00000000000000, 5));
    assert(!vm_is_canonical(0x0100000000000000, 5));
    assert(vm_user_address_valid(0x400000, 3 * VM_PAGE_SIZE, 4));
    assert(!vm_user_address_valid(0x0000800000000000, VM_PAGE_SIZE, 4));
    assert(!vm_user_address_valid(UINT64_MAX - 7, 16, 4));

    uint64_t entry = 0;
    assert(vm_encode_leaf(0x12345000, VM_MAP_WRITE, 40, 1, &entry) == VM_OK);
    assert((entry & 0x12345000) == 0x12345000);
    assert(entry & (UINT64_C(1) << 63)); /* Default data mapping is NX. */
    assert(entry & 2);
    assert(vm_encode_leaf(0x12345000, VM_MAP_EXEC, 40, 1, &entry) == VM_OK);
    assert(!(entry & (UINT64_C(1) << 63)));
    assert(vm_encode_leaf(0x12345000, 0, 40, 0, &entry) == VM_OK);
    assert(!(entry & (UINT64_C(1) << 63))); /* NX is omitted when unsupported. */
    assert(vm_encode_leaf(0x12345001, 0, 40, 1, &entry) == VM_INVALID);
    assert(vm_encode_leaf(UINT64_C(1) << 40, 0, 40, 1, &entry) == VM_INVALID);
    assert(vm_encode_leaf(0x1000, VM_MAP_WRITE | VM_MAP_EXEC, 40, 1, &entry) == VM_INVALID);
    assert(vm_encode_leaf(0x1000, 0x80000000u, 40, 1, &entry) == VM_INVALID);

    test_tables_t tables = {0};
    tables.allocated = 1; /* index zero is the preexisting root */
    vm_space_t space = {
        .root = tables.tables[0], .root_physical = table_physical(0),
        .levels = 4, .physical_bits = 40, .nx_enabled = 1,
        .context = &tables, .resolve = resolve, .allocate = allocate, .release = release
    };
    const uint64_t va = UINT64_C(0x0000004000000000);
    const uint64_t pa = UINT64_C(0x00200000);
    const uint64_t absent_intermediate_va = UINT64_C(0x0000400000200000);
    uint64_t translated = 0;
    /* A fresh root has no intermediate path for the ELF user stack address. */
    assert(vm_core_translate(&space, absent_intermediate_va, &translated, NULL) == VM_NOT_MAPPED);
    assert(vm_core_set_page_cache_flags(&space,absent_intermediate_va,VM_MAP_PCD|VM_MAP_PWT)==VM_NOT_MAPPED);
    /* Translation requires an output address; NULL is invalid, not "unmapped". */
    assert(vm_core_translate(&space, absent_intermediate_va, NULL, NULL) == VM_INVALID);
    assert(vm_core_map(&space, va, pa, VM_MAP_WRITE) == VM_OK);
    uint32_t flags = 0;
    assert(vm_core_translate(&space, va + 0x321, &translated, &flags) == VM_OK);
    assert(translated == pa + 0x321);
    assert((flags & VM_MAP_WRITE) && !(flags & VM_MAP_EXEC) && !(flags & VM_MAP_USER));
    uint64_t alias_va = 0;
    assert(vm_core_find_physical_alias(&space, pa + 0x321, &alias_va, &flags) == VM_OK);
    assert(alias_va == va + 0x321 && (flags & VM_MAP_WRITE));
    assert(vm_core_find_physical_alias(&space, 0x70000000, &alias_va, &flags) == VM_NOT_MAPPED);
    assert(vm_core_find_physical_alias(&space, pa, NULL, &flags) == VM_INVALID);
    space.root[0] &= ~UINT64_C(2);
    assert(vm_core_translate(&space, va, &translated, &flags) == VM_OK);
    assert(!(flags & VM_MAP_WRITE));
    assert((flags & (VM_MAP_PCD | VM_MAP_PWT)) == 0); /* Cache attributes are reported from the mapped leaf. */
    space.root[0] |= UINT64_C(2);
    space.root[0] |= UINT64_C(1) << 63;
    assert(vm_core_translate(&space, va, &translated, &flags) == VM_OK);
    assert(!(flags & VM_MAP_EXEC));
    space.root[0] &= ~(UINT64_C(1) << 63);
    assert(vm_core_map(&space, va, pa, VM_MAP_WRITE) == VM_CONFLICT);
    assert(vm_core_map(&space, 0x0000800000000000, pa, 0) == VM_INVALID);
    assert(vm_core_map(&space, va + 1, pa, 0) == VM_INVALID);
    assert(vm_core_set_page_cache_flags(&space,va,VM_MAP_WRITE)==VM_INVALID);
    assert(vm_core_set_page_cache_flags(&space,va+1,VM_MAP_PCD)==VM_INVALID);
    assert(vm_core_map(&space, va + VM_PAGE_SIZE, UINT64_C(1) << 40, 0) == VM_INVALID);

    uint64_t user_va = va + VM_PAGE_SIZE;
    assert(vm_core_map(&space, user_va, pa + VM_PAGE_SIZE, VM_MAP_USER | VM_MAP_EXEC | VM_MAP_PCD | VM_MAP_PWT) == VM_OK);
    assert(vm_core_translate(&space, user_va, &translated, &flags) == VM_OK);
    assert(translated == pa + VM_PAGE_SIZE && (flags & VM_MAP_USER) && (flags & VM_MAP_EXEC));
    assert(!(flags & VM_MAP_WRITE));
    assert((flags & (VM_MAP_PCD | VM_MAP_PWT)) == (VM_MAP_PCD | VM_MAP_PWT));
    assert(vm_core_set_page_cache_flags(&space,user_va,0)==VM_OK);
    assert(vm_core_translate(&space,user_va,&translated,&flags)==VM_OK);
    assert((flags&(VM_MAP_PCD|VM_MAP_PWT))==0);
    assert(vm_core_set_page_cache_flags(&space,user_va,VM_MAP_PCD|VM_MAP_PWT)==VM_OK);
    assert(vm_core_translate(&space, va, &translated, &flags) == VM_OK);
    assert(!(flags & VM_MAP_USER)); /* User ancestors do not make a supervisor leaf user-accessible. */
    assert(vm_core_map(&space, UINT64_C(0xFFFF800000000000), pa,
                       VM_MAP_USER | VM_MAP_EXEC) == VM_INVALID);

    uint32_t table_count_before = tables.allocated;
    tables.fail_on_call = tables.allocate_calls + 2;
    assert(vm_core_map(&space, UINT64_C(0x0000008000000000), pa, 0) == VM_NO_MEMORY);
    assert(tables.allocated == table_count_before);
    assert(space.root[1] == 0); /* Failed path construction never became reachable. */
    tables.fail_on_call = 0;

    assert(vm_core_unmap(&space, va) == VM_OK);
    assert(vm_core_translate(&space, va, &translated, NULL) == VM_NOT_MAPPED);
    assert(vm_core_unmap(&space, va) == VM_NOT_OWNED);
    assert(vm_core_unmap(&space, va + 1) == VM_INVALID);
    assert(vm_core_unmap(&space, user_va) == VM_OK);
    assert(space.owned_mapping_count == 0);

    uint64_t *pdpt = resolve(&tables, space.root[0] & vm_physical_mask(40));
    assert(pdpt != NULL);
    uint16_t pdpt_index = (uint16_t)((va >> 30) & 0x1FF);
    pdpt[pdpt_index] = UINT64_C(0x40000000) | UINT64_C(0x1083); /* 1 GiB leaf with PAT set. */
    assert(vm_core_translate(&space, va + 0x1234, &translated, &flags) == VM_OK);
    assert(translated == UINT64_C(0x40000000) + (va & ((UINT64_C(1) << 30) - 1)) + 0x1234);
    assert(flags & VM_MAP_WRITE);
    assert(vm_core_find_physical_alias(&space, UINT64_C(0x40000000) + (va & ((UINT64_C(1) << 30) - 1)) + 0x1234, &alias_va, &flags) == VM_OK);
    assert(alias_va == va + 0x1234);
    uint64_t huge_translation=0;
    uint32_t tables_before_cache_split=tables.allocated;
    tables.fail_on_call=tables.allocate_calls+2;
    assert(vm_core_set_page_cache_flags(&space,va+0x3000,VM_MAP_PCD|VM_MAP_PWT)==VM_NO_MEMORY);
    assert(tables.allocated==tables_before_cache_split);
    assert(vm_core_translate(&space,va+0x3000,&huge_translation,&flags)==VM_OK);
    assert(huge_translation==UINT64_C(0x40000000)+(va&((UINT64_C(1)<<30)-1))+0x3000);
    assert((flags&(VM_MAP_PCD|VM_MAP_PWT))==0);
    tables.fail_on_call=0;
    assert(vm_core_set_page_cache_flags(&space,va+0x3000,VM_MAP_PCD|VM_MAP_PWT)==VM_OK);
    assert(vm_core_translate(&space,va+0x3000,&huge_translation,&flags)==VM_OK);
    assert(huge_translation==UINT64_C(0x40000000)+(va&((UINT64_C(1)<<30)-1))+0x3000);
    assert((flags&(VM_MAP_PCD|VM_MAP_PWT))==(VM_MAP_PCD|VM_MAP_PWT));
    uint64_t *split_pd=resolve(&tables,pdpt[pdpt_index]&vm_physical_mask(40));
    assert(split_pd!=NULL);
    uint64_t split_pde=split_pd[(va>>21)&0x1FF];
    uint64_t *split_pt=resolve(&tables,split_pde&vm_physical_mask(40));
    assert(split_pt!=NULL && (split_pt[(va>>12)&0x1FF]&UINT64_C(0x80))); /* Large PAT bit 12 became 4 KiB PAT bit 7. */
    assert((split_pd[((va>>21)+1)&0x1FF]&UINT64_C(0x1000))!=0); /* Neighboring 2 MiB PAT is preserved. */
    assert(vm_core_translate(&space,va+0x4000,&huge_translation,&flags)==VM_OK);
    assert((flags&(VM_MAP_PCD|VM_MAP_PWT))==0); /* Other 4 KiB pages retain the 1 GiB mapping's cache mode. */
    assert(vm_core_set_page_cache_flags(&space,va+(UINT64_C(1)<<21)+0x5000,VM_MAP_PCD|VM_MAP_PWT)==VM_OK);
    assert(vm_core_translate(&space,va+(UINT64_C(1)<<21)+0x5000,&huge_translation,&flags)==VM_OK);
    assert(huge_translation==UINT64_C(0x40000000)+(va&((UINT64_C(1)<<30)-1))+(UINT64_C(1)<<21)+0x5000);
    assert((flags&(VM_MAP_PCD|VM_MAP_PWT))==(VM_MAP_PCD|VM_MAP_PWT)); /* Demotes an untouched 2 MiB leaf. */
    assert((flags&(VM_MAP_WRITE|VM_MAP_EXEC))==(VM_MAP_WRITE|VM_MAP_EXEC));
    assert(vm_core_translate(&space,va+(UINT64_C(1)<<21)+0x6000,&huge_translation,&flags)==VM_OK);
    assert((flags&(VM_MAP_PCD|VM_MAP_PWT))==0);
    assert(huge_translation==UINT64_C(0x40000000)+(va&((UINT64_C(1)<<30)-1))+(UINT64_C(1)<<21)+0x6000);
    assert(vm_core_map(&space, va, pa, 0) == VM_CONFLICT);

    /* LA57 uses the same four lower levels with a wider canonical range. */
    memset(&tables, 0, sizeof(tables));
    tables.allocated = 1;
    space.root = tables.tables[0]; space.root_physical = table_physical(0);
    space.levels = 5; space.context = &tables; space.owned_mapping_count = 0;
    assert(vm_core_map(&space, UINT64_C(0x0080000000000000), pa, 0) == VM_OK);
    assert(vm_core_translate(&space, UINT64_C(0x0080000000000000), &translated, NULL) == VM_OK);
    assert(translated == pa);

    /* Construct an independent root and prove the same VA resolves privately. */
    uint64_t process_root_pa = 0;
    uint64_t *process_root = NULL;
    assert(allocate(&tables, &process_root_pa, &process_root));
    vm_space_t process_space;
    assert(vm_core_space_init(&process_space, process_root, process_root_pa,
                              5, 40, 1, &tables, resolve, allocate, release) == VM_OK);
    const uint64_t isolated_va = UINT64_C(0x0080000000001000);
    assert(vm_core_map(&process_space, isolated_va, pa + 2 * VM_PAGE_SIZE,
                       VM_MAP_USER | VM_MAP_WRITE) == VM_OK);
    assert(vm_core_translate(&process_space, isolated_va, &translated, &flags) == VM_OK);
    assert(translated == pa + 2 * VM_PAGE_SIZE && (flags & VM_MAP_USER) &&
           (flags & VM_MAP_WRITE) && !(flags & VM_MAP_EXEC));
    assert(vm_core_translate(&space, isolated_va, &translated, NULL) == VM_NOT_MAPPED);
    assert(vm_core_space_init(&process_space, process_root, process_root_pa,
                              3, 40, 1, &tables, resolve, allocate, release) == VM_INVALID);
    return 0;
}
