#include <stddef.h>
#include <stdint.h>
#include "fs_core.h"
#define AOSFS_SECTOR_SIZE 512u
#define AOSFS_HEADER_SIZE 36u
#define AOSFS_ENTRY_SIZE 64u
static const uint8_t aosfs_magic[8] = {'A','O','S','F','S','1',0,0};
static int bytes_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
uint32_t fs_checksum(const void *data, size_t size) {
    const uint8_t *p = data; uint32_t h = UINT32_C(2166136261);
    for (size_t i=0;i<size;++i) h=(h^p[i])*UINT32_C(16777619);
    return h;
}
static uint32_t header_checksum(const uint8_t *p) {
    uint32_t h=UINT32_C(2166136261);
    for(uint32_t i=0;i<AOSFS_HEADER_SIZE;++i) {
        uint8_t b=(i>=32&&i<36)?0:p[i]; h=(h^b)*UINT32_C(16777619);
    }
    return h;
}
static int valid_name_bytes(const uint8_t *src, char out[AOSFS_NAME_CAPACITY]) {
    uint32_t n=0; while(n<AOSFS_NAME_CAPACITY&&src[n]) ++n;
    if(!n||n==AOSFS_NAME_CAPACITY) return 0;
    for(uint32_t i=0;i<n;++i) {
        uint8_t c=src[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-')) return 0;
        out[i]=(char)c;
    }
    for(uint32_t i=n;i<AOSFS_NAME_CAPACITY;++i) { if(src[i]) return 0; out[i]=0; }
    if((n==1&&out[0]=='.')||(n==2&&out[0]=='.'&&out[1]=='.')) return 0;
    return 1;
}
static int name_equal(const char *a,const char *b) {
    if (!a || !b) return 0;
    size_t i = 0;
    for(;i<AOSFS_NAME_CAPACITY&&a[i]&&b[i];++i) if(a[i]!=b[i]) return 0;
    return i<AOSFS_NAME_CAPACITY&&a[i]==0&&b[i]==0;
}
int fs_mount(fs_volume_t *volume, block_device_t *device) {
    if(!volume||!device||device->sector_size!=AOSFS_SECTOR_SIZE||device->sector_count<2) return FS_INVALID;
    volume->mounted=0; volume->device=0; volume->file_count=0; volume->total_sectors=0; volume->data_start=0;
    uint8_t header[AOSFS_SECTOR_SIZE], directory[AOSFS_SECTOR_SIZE];
    if(block_read(device,0,1,header)!=BLOCK_OK) return FS_IO_ERROR;
    if(!bytes_equal(header,aosfs_magic,sizeof(aosfs_magic)) ||
       rd16(header+8)!=1 || rd16(header+10)!=AOSFS_SECTOR_SIZE ||
       rd32(header+12)!=AOSFS_HEADER_SIZE || rd32(header+32)!=header_checksum(header)) return FS_CORRUPT;
    for(uint32_t i=AOSFS_HEADER_SIZE;i<AOSFS_SECTOR_SIZE;++i) if(header[i]) return FS_CORRUPT;
    uint32_t total=rd32(header+16),dir_lba=rd32(header+20),data_start=rd32(header+28);
    uint16_t count=rd16(header+24),entry_size=rd16(header+26);
    if(!total||total>device->sector_count||dir_lba!=1||count>AOSFS_MAX_FILES||entry_size!=AOSFS_ENTRY_SIZE||
       data_start<2||data_start>total) return FS_CORRUPT;
    if(block_read(device,dir_lba,1,directory)!=BLOCK_OK) return FS_IO_ERROR;
    for(uint32_t i=0;i<count;++i) {
        const uint8_t *row=directory+i*AOSFS_ENTRY_SIZE; fs_file_t *file=&volume->files[i];
        if(!valid_name_bytes(row,file->name)||rd32(row+32)!=1) return FS_CORRUPT;
        file->first_sector=rd32(row+36); file->byte_size=rd32(row+40);
        file->sector_count=rd32(row+44); file->checksum=rd32(row+48);
        for(uint32_t z=52;z<AOSFS_ENTRY_SIZE;++z) if(row[z]) return FS_CORRUPT;
        uint64_t expected=((uint64_t)file->byte_size+AOSFS_SECTOR_SIZE-1)/AOSFS_SECTOR_SIZE;
        if(!file->byte_size||expected!=file->sector_count||file->first_sector<data_start||
           file->first_sector>total||file->sector_count>total-file->first_sector) return FS_CORRUPT;
        for(uint32_t j=0;j<i;++j) {
            const fs_file_t *prior=&volume->files[j];
            if(name_equal(file->name,prior->name)) return FS_CORRUPT;
            uint64_t end=(uint64_t)file->first_sector+file->sector_count;
            uint64_t prior_end=(uint64_t)prior->first_sector+prior->sector_count;
            if(file->first_sector<prior_end&&prior->first_sector<end) return FS_CORRUPT;
        }
    }
    for(uint32_t i=count*AOSFS_ENTRY_SIZE;i<AOSFS_SECTOR_SIZE;++i) if(directory[i]) return FS_CORRUPT;
    volume->device=device; volume->total_sectors=total; volume->data_start=data_start;
    volume->file_count=count; volume->mounted=1; return FS_OK;
}
uint32_t fs_file_count(const fs_volume_t *volume) { return volume&&volume->mounted?volume->file_count:0; }
const fs_file_t *fs_file_at(const fs_volume_t *volume,uint32_t index) {
    return volume&&volume->mounted&&index<volume->file_count?&volume->files[index]:0;
}
const fs_file_t *fs_lookup(const fs_volume_t *volume,const char *name) {
    if(!volume||!volume->mounted||!name) return 0;
    for(uint32_t i=0;i<volume->file_count;++i) if(name_equal(volume->files[i].name,name)) return &volume->files[i];
    return 0;
}
int fs_read_file(const fs_volume_t *volume,const char *name,void *buffer,size_t capacity,size_t *bytes_read) {
    if(bytes_read) *bytes_read=0;
    if(!volume||!volume->mounted||!name||!buffer||!bytes_read) return FS_INVALID;
    const fs_file_t *file=fs_lookup(volume,name); if(!file) return FS_NOT_FOUND;
    if(file->byte_size>capacity) return FS_BUFFER_TOO_SMALL;
    uint8_t sector[AOSFS_SECTOR_SIZE]; uint32_t hash=UINT32_C(2166136261); size_t copied=0;
    for(uint32_t i=0;i<file->sector_count;++i) {
        if(block_read(volume->device,(uint64_t)file->first_sector+i,1,sector)!=BLOCK_OK) return FS_IO_ERROR;
        size_t remain=(size_t)file->byte_size-copied; size_t take=remain<AOSFS_SECTOR_SIZE?remain:AOSFS_SECTOR_SIZE;
        uint8_t *dst=(uint8_t*)buffer+copied;
        for(size_t j=0;j<take;++j) { dst[j]=sector[j]; hash=(hash^sector[j])*UINT32_C(16777619); }
        copied+=take;
    }
    if(copied!=file->byte_size||hash!=file->checksum) return FS_CORRUPT;
    *bytes_read=copied; return FS_OK;
}
