#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/elf64_loader.h"

#define IMAGE_SIZE 0x3005u
#define MAX_MOCK_PAGES 8u
#define MOCK_BASE UINT64_C(0x200000)
#define USER_BASE UINT64_C(0x400000)
#define ELF_PT_DYNAMIC 2u

typedef struct {
    uint8_t bytes[MAX_MOCK_PAGES][VM_PAGE_SIZE];
    uint64_t va[MAX_MOCK_PAGES];
    uint64_t pa[MAX_MOCK_PAGES];
    uint32_t flags[MAX_MOCK_PAGES];
    uint32_t allocated;
    uint32_t mapped;
    uint32_t released;
    uint32_t fail_map_after;
    uint32_t fail_alloc_after;
} mock_memory_t;

static void put16(uint8_t *p, uint16_t value) { p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8); }
static void put32(uint8_t *p, uint32_t value) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(value>>(i*8)); }
static void put64(uint8_t *p, uint64_t value) { for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(value>>(i*8)); }

static void init_elf(uint8_t image[IMAGE_SIZE]) {
    memset(image, 0, IMAGE_SIZE);
    image[0]=0x7f; image[1]='E'; image[2]='L'; image[3]='F';
    image[4]=2; image[5]=1; image[6]=1; image[7]=0;
    put16(image+16, 2); put16(image+18, 62); put32(image+20, 1);
    put64(image+24, USER_BASE); put64(image+32, 64);
    put16(image+52, 64); put16(image+54, 56); put16(image+56, 2);

    uint8_t *text=image+64;
    put32(text, 1); put32(text+4, 5); put64(text+8, 0x1000);
    put64(text+16, USER_BASE); put64(text+32, 5); put64(text+40, 0x1800); put64(text+48, 0x1000);
    uint8_t *data=text+56;
    put32(data, 1); put32(data+4, 6); put64(data+8, 0x2000);
    put64(data+16, USER_BASE+0x2000); put64(data+32, 5); put64(data+40, 0x1000); put64(data+48, 0x1000);
    image[0x1000]=0xB8; image[0x1001]=0x2A; image[0x1002]=0; image[0x1003]=0; image[0x1004]=0;
    image[0x2000]=1; image[0x2001]=2; image[0x2002]=3; image[0x2003]=4; image[0x2004]=5;
}

static int mock_unmapped(void *ctx, uint64_t va) {
    mock_memory_t *m=ctx;
    for(uint32_t i=0;i<m->mapped;i++) if(m->va[i]==va) return 0;
    return 1;
}
static int mock_alloc(void *ctx, uint64_t *pa, uint8_t **writable) {
    mock_memory_t *m=ctx;
    if(m->allocated>=MAX_MOCK_PAGES || (m->fail_alloc_after && m->allocated>=m->fail_alloc_after)) return 0;
    uint32_t i=m->allocated++;
    memset(m->bytes[i],0xCC,VM_PAGE_SIZE);
    *pa=MOCK_BASE+(uint64_t)i*VM_PAGE_SIZE; *writable=m->bytes[i]; return 1;
}
static int mock_release(void *ctx, uint64_t pa) { mock_memory_t *m=ctx; (void)pa; m->released++; return 1; }
static int mock_map(void *ctx, uint64_t va, uint64_t pa, uint32_t flags) {
    mock_memory_t *m=ctx;
    if(m->fail_map_after && m->mapped>=m->fail_map_after) return 0;
    if(m->mapped>=MAX_MOCK_PAGES || mock_unmapped(ctx,va)!=1 || (flags&VM_MAP_WRITE && flags&VM_MAP_EXEC)) return 0;
    m->va[m->mapped]=va; m->pa[m->mapped]=pa; m->flags[m->mapped]=flags; m->mapped++; return 1;
}
static int mock_unmap(void *ctx, uint64_t va) {
    mock_memory_t *m=ctx;
    for(uint32_t i=0;i<m->mapped;i++) if(m->va[i]==va) { m->mapped--; m->va[i]=m->va[m->mapped]; return 1; }
    return 0;
}
static elf64_memory_ops_t make_ops(mock_memory_t *memory) {
    elf64_memory_ops_t ops={memory,mock_unmapped,mock_alloc,mock_release,mock_map,mock_unmap}; return ops;
}

static void test_parse_and_load(void) {
    uint8_t image[IMAGE_SIZE]; init_elf(image);
    elf64_plan_t plan;
    assert(elf64_parse(image,sizeof(image),4,&plan)==ELF64_OK);
    assert(plan.entry==USER_BASE && plan.segment_count==2 && plan.page_count==3);
    mock_memory_t memory={0}; elf64_memory_ops_t ops=make_ops(&memory); elf64_loaded_image_t loaded;
    assert(elf64_load(image,sizeof(image),4,1,&ops,&loaded)==ELF64_OK);
    assert(loaded.entry==USER_BASE && loaded.page_count==3 && memory.mapped==3);
    assert(memory.va[0]==USER_BASE && memory.pa[0]==MOCK_BASE);
    assert((memory.flags[0]&(VM_MAP_USER|VM_MAP_EXEC))==(VM_MAP_USER|VM_MAP_EXEC));
    assert(!(memory.flags[0]&VM_MAP_WRITE));
    assert(memory.bytes[0][0]==0xB8 && memory.bytes[0][4]==0);
    assert(memory.bytes[0][5]==0 && memory.bytes[1][VM_PAGE_SIZE-1]==0);
    assert(memory.flags[2]&VM_MAP_WRITE); assert(!(memory.flags[2]&VM_MAP_EXEC));
    assert(memory.bytes[2][0]==1 && memory.bytes[2][4]==5 && memory.bytes[2][5]==0);
    assert(elf64_unload(&ops,&loaded)==ELF64_OK);
    assert(loaded.page_count==0 && loaded.entry==0 && memory.mapped==0 && memory.released==3);
}

static void expect_invalid(uint8_t image[IMAGE_SIZE], elf64_status_t expected) {
    elf64_plan_t plan;
    assert(elf64_parse(image,IMAGE_SIZE,4,&plan)==expected);
}
static void test_bad_headers(void) {
    uint8_t image[IMAGE_SIZE]; init_elf(image);
    elf64_plan_t plan;
    assert(elf64_parse(image,63,4,&plan)==ELF64_INVALID);
    image[0]=0; expect_invalid(image,ELF64_INVALID); init_elf(image);
    put16(image+52,63); expect_invalid(image,ELF64_INVALID); init_elf(image);
    put64(image+32,UINT64_MAX-8); expect_invalid(image,ELF64_INVALID); init_elf(image);
    put64(image+32,IMAGE_SIZE-32); expect_invalid(image,ELF64_INVALID); init_elf(image);
    put64(image+32,65); expect_invalid(image,ELF64_INVALID); init_elf(image);
    put16(image+54,55); expect_invalid(image,ELF64_INVALID); init_elf(image);
    put16(image+56,0); expect_invalid(image,ELF64_INVALID); init_elf(image);
    put16(image+16,3); expect_invalid(image,ELF64_UNSUPPORTED); init_elf(image);
    put16(image+18,3); expect_invalid(image,ELF64_UNSUPPORTED); init_elf(image);
    image[5]=2; expect_invalid(image,ELF64_INVALID); init_elf(image);
    image[8]=1; expect_invalid(image,ELF64_INVALID); init_elf(image);
    put64(image+24,USER_BASE+0x4000); expect_invalid(image,ELF64_INVALID);
}
static void test_bad_segments(void) {
    uint8_t image[IMAGE_SIZE]; init_elf(image); uint8_t *ph=image+64;
    put64(ph+32,0x2000); expect_invalid(image,ELF64_INVALID); init_elf(image); ph=image+64;
    put64(ph+8,IMAGE_SIZE-2); put64(ph+32,5); expect_invalid(image,ELF64_INVALID); init_elf(image); ph=image+64;
    put64(ph+40,UINT64_MAX); expect_invalid(image,ELF64_INVALID); init_elf(image); ph=image+64;
    put64(ph+16,UINT64_C(0x00007ffffffffff0)); put64(ph+40,0x2000); put64(ph+8,0x1ff0); expect_invalid(image,ELF64_INVALID); init_elf(image); ph=image+64;
    put32(ph+4,7); expect_invalid(image,ELF64_INVALID); init_elf(image); ph=image+64;
    put64(ph+16,0); expect_invalid(image,ELF64_INVALID); init_elf(image); ph=image+64;
    put32(ph+4,4); expect_invalid(image,ELF64_INVALID); init_elf(image); ph=image+64;
    put32(ph+4,13); expect_invalid(image,ELF64_INVALID); init_elf(image); ph=image+64;
    put64(ph+48,3); expect_invalid(image,ELF64_INVALID); init_elf(image); ph=image+64;
    put64(ph+48,0x1000); put64(ph+8,0x1001); expect_invalid(image,ELF64_INVALID); init_elf(image); ph=image+64;
    put32(ph,ELF_PT_DYNAMIC); expect_invalid(image,ELF64_UNSUPPORTED); init_elf(image); ph=image+64;
    put16(image+56,3); ph=image+64+112; put32(ph,0x6474E551u); put32(ph+4,7); expect_invalid(image,ELF64_UNSUPPORTED); init_elf(image); ph=image+64;
    put32(ph+16,USER_BASE+0x1000); /* second PT_LOAD now shares a page with the first */
    put64(ph+8,0x1000); put64(ph+32,5); put64(ph+40,0x1800); put64(ph+48,0x1000);
    put64(image+64+56+8,0x2000); put64(image+64+56+16,USER_BASE+0x1000); expect_invalid(image,ELF64_UNSUPPORTED);
    init_elf(image); put16(image+56,1); put64(ph+40,(uint64_t)257*VM_PAGE_SIZE); expect_invalid(image,ELF64_UNSUPPORTED);
}
static void test_load_rejections_and_rollback(void) {
    uint8_t image[IMAGE_SIZE]; init_elf(image);
    mock_memory_t memory={0}; elf64_memory_ops_t ops=make_ops(&memory); elf64_loaded_image_t loaded;
    assert(elf64_load(image,sizeof(image),4,0,&ops,&loaded)==ELF64_UNSUPPORTED);
    assert(memory.allocated==0);
    memory.fail_map_after=1;
    assert(elf64_load(image,sizeof(image),4,1,&ops,&loaded)==ELF64_MAPPING_FAILED);
    assert(memory.mapped==0 && memory.released==2 && loaded.page_count==0);
    memory=(mock_memory_t){0}; ops=make_ops(&memory);
    memory.va[0]=USER_BASE; memory.mapped=1;
    assert(elf64_load(image,sizeof(image),4,1,&ops,&loaded)==ELF64_MAPPING_FAILED);
    assert(memory.allocated==0 && memory.mapped==1);
    memory=(mock_memory_t){0}; ops=make_ops(&memory); memory.fail_alloc_after=1;
    assert(elf64_load(image,sizeof(image),4,1,&ops,&loaded)==ELF64_NO_MEMORY);
    assert(memory.mapped==0 && memory.released==1 && loaded.page_count==0);
}
static void test_compiled_hello_elf(const char *path) {
    FILE *file = fopen(path, "rb"); assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file); assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *image = (uint8_t *)malloc((size_t)length); assert(image);
    assert(fread(image, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    elf64_plan_t plan;
    assert(elf64_parse(image, (size_t)length, 4, &plan) == ELF64_OK);
    assert(plan.entry == UINT64_C(0x0000400000000000));
    assert(plan.segment_count == 1 && plan.page_count == 1);
    assert(plan.segments[0].flags == 5u);
    mock_memory_t memory = {0}; elf64_memory_ops_t ops = make_ops(&memory);
    elf64_loaded_image_t loaded;
    assert(elf64_load(image, (size_t)length, 4, 1, &ops, &loaded) == ELF64_OK);
    assert(loaded.entry == plan.entry && loaded.page_count == 1);
    assert(memory.flags[0] == (VM_MAP_USER | VM_MAP_EXEC));
    static const char message[] = "Hello from independently assembled hello.elf via syscall write.\r\n";
    int found = 0;
    for (size_t i = 0; i + sizeof(message) - 1 <= VM_PAGE_SIZE; ++i)
        if (memcmp(memory.bytes[0] + i, message, sizeof(message) - 1) == 0) found = 1;
    assert(found);
    assert(elf64_unload(&ops, &loaded) == ELF64_OK);
    assert(memory.mapped == 0 && memory.released == 1);
    free(image);
}

int main(int argc, char **argv) {
    test_parse_and_load(); test_bad_headers(); test_bad_segments(); test_load_rejections_and_rollback();
    if (argc == 2) test_compiled_hello_elf(argv[1]);
    puts("ELF64 loader tests passed"); return 0;
}














