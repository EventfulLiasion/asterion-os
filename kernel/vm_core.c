#include "vm_core.h"

#define PTE_PRESENT UINT64_C(0x001)
#define PTE_WRITE   UINT64_C(0x002)
#define PTE_USER    UINT64_C(0x004)
#define PTE_PWT     UINT64_C(0x008)
#define PTE_PCD     UINT64_C(0x010)
#define PTE_HUGE    UINT64_C(0x080)
#define PTE_GLOBAL  UINT64_C(0x100)
#define PTE_NX      (UINT64_C(1) << 63)
#define VM_ALLOWED_FLAGS (VM_MAP_WRITE | VM_MAP_EXEC | VM_MAP_USER | VM_MAP_PWT | VM_MAP_PCD | VM_MAP_GLOBAL)

uint64_t vm_physical_mask(uint8_t physical_bits) {
    if (physical_bits < 12 || physical_bits > 52) return 0;
    return ((UINT64_C(1) << physical_bits) - 1) & ~(VM_PAGE_SIZE - 1);
}

int vm_is_canonical(uint64_t address, uint8_t levels) {
    uint8_t bits = levels == 4 ? 48 : (levels == 5 ? 57 : 0);
    if (!bits) return 0;
    uint64_t upper = address >> bits;
    uint64_t sign = (address >> (bits - 1)) & 1;
    return sign ? upper == (UINT64_MAX >> bits) : upper == 0;
}

int vm_user_address_valid(uint64_t address, uint64_t size, uint8_t levels) {
    uint8_t bits = levels == 4 ? 48 : (levels == 5 ? 57 : 0);
    if (!bits || !size || !vm_is_canonical(address, levels) ||
        address >= (UINT64_C(1) << (bits - 1)) || size - 1 > UINT64_MAX - address)
        return 0;
    uint64_t end = address + size - 1;
    return end < (UINT64_C(1) << (bits - 1)) && vm_is_canonical(end, levels);
}

int vm_core_space_init(vm_space_t *space, uint64_t *root, uint64_t root_physical,
                       uint8_t levels, uint8_t physical_bits, uint8_t nx_enabled,
                       void *context, vm_resolve_table_fn resolve,
                       vm_allocate_table_fn allocate, vm_release_table_fn release) {
    if (!space || !root || (root_physical & (VM_PAGE_SIZE - 1)) ||
        (levels != 4 && levels != 5) || !vm_physical_mask(physical_bits) ||
        (root_physical & ~vm_physical_mask(physical_bits)) ||
        !resolve || !allocate || !release) return VM_INVALID;
    space->root = root;
    space->root_physical = root_physical;
    space->levels = levels;
    space->physical_bits = physical_bits;
    space->nx_enabled = nx_enabled != 0;
    space->reserved = 0;
    space->context = context;
    space->resolve = resolve;
    space->allocate = allocate;
    space->release = release;
    space->owned_mapping_count = 0;
    space->owned_table_count = 0;
    for (uint32_t i = 0; i < VM_MAX_OWNED_MAPPINGS; ++i)
        space->owned_virtual_pages[i] = 0;
    return VM_OK;
}

int vm_encode_leaf(uint64_t physical, uint32_t flags, uint8_t physical_bits,
                   uint8_t nx_enabled, uint64_t *entry) {
    uint64_t mask = vm_physical_mask(physical_bits);
    if (!entry || !mask || (physical & (VM_PAGE_SIZE - 1)) || (physical & ~mask) ||
        (flags & ~VM_ALLOWED_FLAGS) ||
        ((flags & (VM_MAP_WRITE | VM_MAP_EXEC)) == (VM_MAP_WRITE | VM_MAP_EXEC)))
        return VM_INVALID;
    uint64_t encoded = physical | PTE_PRESENT;
    if (flags & VM_MAP_WRITE) encoded |= PTE_WRITE;
    if (flags & VM_MAP_USER) encoded |= PTE_USER;
    if (flags & VM_MAP_PWT) encoded |= PTE_PWT;
    if (flags & VM_MAP_PCD) encoded |= PTE_PCD;
    if (flags & VM_MAP_GLOBAL) encoded |= PTE_GLOBAL;
    if (nx_enabled && !(flags & VM_MAP_EXEC)) encoded |= PTE_NX;
    *entry = encoded;
    return VM_OK;
}

static uint16_t level_index(uint64_t address, uint8_t level) {
    return (uint16_t)((address >> (12 + (uint32_t)(level - 1) * 9)) & 0x1FF);
}

static int space_valid(const vm_space_t *space) {
    return space && space->root && (space->levels == 4 || space->levels == 5) &&
           vm_physical_mask(space->physical_bits) && space->resolve;
}

static int is_tracked(const vm_space_t *space, uint64_t address, uint32_t *index) {
    uint64_t page = address / VM_PAGE_SIZE;
    for (uint32_t i = 0; i < space->owned_mapping_count; ++i) {
        if (space->owned_virtual_pages[i] == page) {
            if (index) *index = i;
            return 1;
        }
    }
    return 0;
}

static uint32_t leaf_flags(uint64_t entry, uint8_t nx_enabled) {
    uint32_t flags = 0;
    if (entry & PTE_WRITE) flags |= VM_MAP_WRITE;
    if (entry & PTE_USER) flags |= VM_MAP_USER;
    if (entry & PTE_PWT) flags |= VM_MAP_PWT;
    if (entry & PTE_PCD) flags |= VM_MAP_PCD;
    if (entry & PTE_GLOBAL) flags |= VM_MAP_GLOBAL;
    if (!nx_enabled || !(entry & PTE_NX)) flags |= VM_MAP_EXEC;
    return flags;
}

int vm_core_translate(const vm_space_t *space, uint64_t address,
                      uint64_t *physical_address, uint32_t *flags) {
    if (!space_valid(space) || !vm_is_canonical(address, space->levels) || !physical_address)
        return VM_INVALID;
    uint64_t mask = vm_physical_mask(space->physical_bits);
    uint64_t *table = space->root;
    int writable = 1, user = 1, executable = 1;
    for (uint8_t level = space->levels; level > 0; --level) {
        uint64_t entry = table[level_index(address, level)];
        if (!(entry & PTE_PRESENT)) return VM_NOT_MAPPED;
        if (!(entry & PTE_WRITE)) writable = 0;
        if (!(entry & PTE_USER)) user = 0;
        if (space->nx_enabled && (entry & PTE_NX)) executable = 0;
        if ((level == 3 || level == 2) && (entry & PTE_HUGE)) {
            uint32_t shift = level == 3 ? 30 : 21;
            uint64_t page_mask = (UINT64_C(1) << shift) - 1;
            *physical_address = (entry & mask & ~page_mask) | (address & page_mask);
            if (flags) {
                *flags = leaf_flags(entry, space->nx_enabled);
                if (!writable) *flags &= ~VM_MAP_WRITE;
                if (!user) *flags &= ~VM_MAP_USER;
                if (!executable) *flags &= ~VM_MAP_EXEC;
            }
            return VM_OK;
        }
        if (level == 1) {
            *physical_address = (entry & mask) | (address & (VM_PAGE_SIZE - 1));
            if (flags) {
                *flags = leaf_flags(entry, space->nx_enabled);
                if (!writable) *flags &= ~VM_MAP_WRITE;
                if (!user) *flags &= ~VM_MAP_USER;
                if (!executable) *flags &= ~VM_MAP_EXEC;
            }
            return VM_OK;
        }
        uint64_t child_physical = entry & mask;
        table = space->resolve(space->context, child_physical);
        if (!table) return VM_CORRUPT_TABLE;
    }
    return VM_CORRUPT_TABLE;
}

#define PTE_ACCESSED UINT64_C(0x020)
#define PTE_DIRTY UINT64_C(0x040)
#define PTE_PAT_LARGE (UINT64_C(1) << 12)
#define PTE_SOFTWARE_MASK ((UINT64_C(7) << 9) | (UINT64_C(0x7F) << 52))

static int allocate_split_table(vm_space_t *space,uint64_t *physical,uint64_t **table){
    uint64_t pa=0;uint64_t *memory=0;uint64_t mask=vm_physical_mask(space->physical_bits);
    if(!space->allocate||!space->release||!space->allocate(space->context,&pa,&memory))return VM_NO_MEMORY;
    if(!memory||(pa&(VM_PAGE_SIZE-1))||(pa&~mask)){space->release(space->context,pa);return VM_NO_MEMORY;}
    *physical=pa;*table=memory;return VM_OK;
}

static uint64_t table_entry_flags(uint64_t leaf){
    return leaf&(PTE_PRESENT|PTE_WRITE|PTE_USER|PTE_ACCESSED|PTE_NX|PTE_SOFTWARE_MASK);
}

static void expand_large_to_2m(uint64_t *directory,uint64_t entry,uint64_t physical_base,
                               uint64_t physical_mask){
    uint64_t attributes=(entry&~physical_mask)|PTE_HUGE;
    if(entry&PTE_PAT_LARGE)attributes|=PTE_PAT_LARGE;
    for(uint32_t i=0;i<512;++i)
        directory[i]=(physical_base+((uint64_t)i<<21))|attributes;
}

static void expand_large_to_4k(uint64_t *table,uint64_t entry,uint64_t physical_base,
                               uint64_t physical_mask){
    uint64_t attributes=(entry&~physical_mask)&~PTE_HUGE;
    if(entry&PTE_PAT_LARGE)attributes|=PTE_HUGE; /* PAT occupies bit 7 in a 4 KiB PTE. */
    for(uint32_t i=0;i<512;++i)
        table[i]=(physical_base+((uint64_t)i<<12))|attributes;
}

static void replace_leaf_cache_flags(uint64_t *entry,uint32_t cache_flags){
    *entry&=~(PTE_PCD|PTE_PWT);
    if(cache_flags&VM_MAP_PCD)*entry|=PTE_PCD;
    if(cache_flags&VM_MAP_PWT)*entry|=PTE_PWT;
}

int vm_core_set_page_cache_flags(vm_space_t *space,uint64_t address,uint32_t cache_flags){
    if(!space_valid(space)||!space->allocate||!space->release||
       !vm_is_canonical(address,space->levels)||(address&(VM_PAGE_SIZE-1))||
       (cache_flags&~(VM_MAP_PCD|VM_MAP_PWT)))return VM_INVALID;
    uint64_t mask=vm_physical_mask(space->physical_bits),*table=space->root;
    for(uint8_t level=space->levels;level>0;--level){
        uint16_t index=level_index(address,level);uint64_t entry=table[index];
        if(!(entry&PTE_PRESENT))return VM_NOT_MAPPED;
        if((level==3||level==2)&&(entry&PTE_HUGE)){
            uint32_t shift=level==3?30u:21u;uint64_t span=(UINT64_C(1)<<shift)-1;
            uint64_t physical_base=(entry&mask)&~span;
            uint64_t *new_table=0,new_physical=0;int result;
            if(level==3){
                result=allocate_split_table(space,&new_physical,&new_table);
                if(result!=VM_OK)return result;
                expand_large_to_2m(new_table,entry,physical_base,mask);
                uint16_t pde_index=level_index(address,2);uint64_t pde=new_table[pde_index];
                uint64_t *new_pt=0,new_pt_physical=0;
                result=allocate_split_table(space,&new_pt_physical,&new_pt);
                if(result!=VM_OK){space->release(space->context,new_physical);return result;}
                uint64_t pde_base=(pde&mask)&~((UINT64_C(1)<<21)-1);
                expand_large_to_4k(new_pt,pde,pde_base,mask);
                replace_leaf_cache_flags(&new_pt[level_index(address,1)],cache_flags);
                new_table[pde_index]=new_pt_physical|table_entry_flags(pde);
                table[index]=new_physical|table_entry_flags(entry);
                return VM_OK;
            }
            result=allocate_split_table(space,&new_physical,&new_table);
            if(result!=VM_OK)return result;
            expand_large_to_4k(new_table,entry,physical_base,mask);
            replace_leaf_cache_flags(&new_table[level_index(address,1)],cache_flags);
            table[index]=new_physical|table_entry_flags(entry);
            return VM_OK;
        }
        if(level==1){replace_leaf_cache_flags(&table[index],cache_flags);return VM_OK;}
        if(entry&PTE_HUGE)return VM_CORRUPT_TABLE;
        table=space->resolve(space->context,entry&mask);
        if(!table)return VM_CORRUPT_TABLE;
    }
    return VM_CORRUPT_TABLE;
}
static int find_physical_alias_tree(const vm_space_t *space,uint64_t *table,uint8_t level,
                                    uint64_t prefix,uint64_t target,int writable,
                                    int user,int executable,uint64_t *virtual_address,
                                    uint32_t *flags){
    uint64_t mask=vm_physical_mask(space->physical_bits);
    for(uint32_t index=0;index<512;++index){
        uint64_t entry=table[index];if(!(entry&PTE_PRESENT))continue;
        int next_writable=writable&&((entry&PTE_WRITE)!=0);
        int next_user=user&&((entry&PTE_USER)!=0);
        int next_executable=executable&&(!space->nx_enabled||!(entry&PTE_NX));
        int huge=(level==3||level==2)&&(entry&PTE_HUGE);
        if(level==1||huge){
            uint32_t shift=huge?(level==3?30u:21u):12u;
            uint64_t span=UINT64_C(1)<<shift;
            uint64_t physical_base=(entry&mask)&~(span-1);
            if(target<physical_base||target-physical_base>=span)continue;
            uint64_t candidate=prefix|((uint64_t)index<<shift)|(target-physical_base);
            uint8_t canonical_bits=space->levels==4?48:57;
            if(candidate&(UINT64_C(1)<<(canonical_bits-1)))
                candidate|=UINT64_MAX<<canonical_bits;
            *virtual_address=candidate;
            if(flags){
                *flags=leaf_flags(entry,space->nx_enabled);
                if(!next_writable)*flags&=~VM_MAP_WRITE;
                if(!next_user)*flags&=~VM_MAP_USER;
                if(!next_executable)*flags&=~VM_MAP_EXEC;
            }
            return VM_OK;
        }
        uint64_t child_physical=entry&mask;
        uint64_t *child=space->resolve(space->context,child_physical);
        if(!child)return VM_CORRUPT_TABLE;
        int status=find_physical_alias_tree(space,child,(uint8_t)(level-1),
                    prefix|((uint64_t)index<<(12+(uint32_t)(level-1)*9)),target,
                    next_writable,next_user,next_executable,virtual_address,flags);
        if(status==VM_OK||status==VM_CORRUPT_TABLE)return status;
    }
    return VM_NOT_MAPPED;
}

int vm_core_find_physical_alias(const vm_space_t *space,uint64_t physical_address,
                                uint64_t *virtual_address,uint32_t *flags){
    uint64_t mask;
    if(!space_valid(space)||!virtual_address||!(mask=vm_physical_mask(space->physical_bits))||
       (physical_address&~(mask|(VM_PAGE_SIZE-1))))return VM_INVALID;
    return find_physical_alias_tree(space,space->root,space->levels,0,physical_address,
                                    1,1,1,virtual_address,flags);
}
int vm_core_map(vm_space_t *space, uint64_t address, uint64_t physical,
                uint32_t flags) {
    uint64_t ignored;
    if (!space_valid(space) || !space->allocate || !space->release ||
        !vm_is_canonical(address, space->levels) ||
        (address & (VM_PAGE_SIZE - 1)) ||
        ((flags & VM_MAP_USER) && !vm_user_address_valid(address, VM_PAGE_SIZE, space->levels)) ||
        vm_encode_leaf(physical, flags, space->physical_bits, space->nx_enabled, &ignored) != VM_OK)
        return VM_INVALID;
    if (space->owned_mapping_count >= VM_MAX_OWNED_MAPPINGS) return VM_CAPACITY;
    uint32_t existing_index;
    if (is_tracked(space, address, &existing_index)) return VM_CONFLICT;

    uint64_t *parents[5] = {0};
    uint16_t parent_indexes[5] = {0};
    uint64_t *table = space->root;
    uint8_t first_missing = 0;
    for (uint8_t level = space->levels; level > 1; --level) {
        uint16_t index = level_index(address, level);
        uint64_t entry = table[index];
        parents[level - 1] = table;
        parent_indexes[level - 1] = index;
        if (!(entry & PTE_PRESENT)) { first_missing = level; break; }
        if ((level == 3 || level == 2) && (entry & PTE_HUGE)) return VM_CONFLICT;
        table = space->resolve(space->context, entry & vm_physical_mask(space->physical_bits));
        if (!table) return VM_CORRUPT_TABLE;
    }

    uint64_t *new_tables[4] = {0};
    uint64_t new_physical[4] = {0};
    uint8_t new_count = 0;
    if (first_missing) {
        for (uint8_t level = first_missing - 1; level > 0; --level) {
            uint64_t new_page = 0;
            uint64_t *new_table = 0;
            if (!space->allocate(space->context, &new_page, &new_table)) {
                for (uint8_t i = 0; i < new_count; ++i)
                    space->release(space->context, new_physical[i]);
                return VM_NO_MEMORY;
            }
            if (!new_table || (new_page & (VM_PAGE_SIZE - 1)) ||
                (new_page & ~vm_physical_mask(space->physical_bits))) {
                space->release(space->context, new_page);
                for (uint8_t i = 0; i < new_count; ++i)
                    space->release(space->context, new_physical[i]);
                return VM_NO_MEMORY;
            }
            new_physical[new_count] = new_page;
            new_tables[new_count] = new_table;
            ++new_count;
        }
        /* Link the newly allocated tables together while they are unreachable. */
        for (uint8_t i = 0; i + 1 < new_count; ++i) {
            uint8_t level = first_missing - 1 - i;
            new_tables[i][level_index(address, level)] =
                new_physical[i + 1] | PTE_PRESENT | PTE_WRITE |
                ((flags & VM_MAP_USER) ? PTE_USER : 0);
        }
        table = new_tables[new_count - 1];
    }

    uint16_t leaf_index = level_index(address, 1);
    if (table[leaf_index] & PTE_PRESENT) {
        for (uint8_t i = 0; i < new_count; ++i)
            space->release(space->context, new_physical[i]);
        return VM_CONFLICT;
    }
    uint64_t leaf_entry = 0;
    (void)vm_encode_leaf(physical, flags, space->physical_bits, space->nx_enabled, &leaf_entry);
    table[leaf_index] = leaf_entry;

    if (first_missing) {
        uint64_t parent_entry = new_physical[0] | PTE_PRESENT | PTE_WRITE |
                                ((flags & VM_MAP_USER) ? PTE_USER : 0);
        parents[first_missing - 1][parent_indexes[first_missing - 1]] = parent_entry;
        for (uint8_t level = first_missing; level < space->levels; ++level) {
            uint64_t *parent = parents[level];
            if (flags & VM_MAP_WRITE) parent[parent_indexes[level]] |= PTE_WRITE;
            if (flags & VM_MAP_USER) parent[parent_indexes[level]] |= PTE_USER;
        }
    } else {
        for (uint8_t level = 1; level < space->levels; ++level) {
            uint64_t *parent = parents[level];
            if (flags & VM_MAP_WRITE) parent[parent_indexes[level]] |= PTE_WRITE;
            if (flags & VM_MAP_USER) parent[parent_indexes[level]] |= PTE_USER;
        }
    }
    space->owned_virtual_pages[space->owned_mapping_count++] = address / VM_PAGE_SIZE;
    return VM_OK;
}

int vm_core_unmap(vm_space_t *space, uint64_t address) {
    if (!space_valid(space) || !vm_is_canonical(address, space->levels) ||
        (address & (VM_PAGE_SIZE - 1))) return VM_INVALID;
    uint32_t tracked_index;
    if (!is_tracked(space, address, &tracked_index)) return VM_NOT_OWNED;
    uint64_t *table = space->root;
    for (uint8_t level = space->levels; level > 1; --level) {
        uint64_t entry = table[level_index(address, level)];
        if (!(entry & PTE_PRESENT) || ((level == 3 || level == 2) && (entry & PTE_HUGE)))
            return VM_CORRUPT_TABLE;
        table = space->resolve(space->context, entry & vm_physical_mask(space->physical_bits));
        if (!table) return VM_CORRUPT_TABLE;
    }
    uint16_t index = level_index(address, 1);
    if (!(table[index] & PTE_PRESENT)) return VM_CORRUPT_TABLE;
    table[index] = 0;
    --space->owned_mapping_count;
    if (tracked_index != space->owned_mapping_count)
        space->owned_virtual_pages[tracked_index] = space->owned_virtual_pages[space->owned_mapping_count];
    return VM_OK;
}
