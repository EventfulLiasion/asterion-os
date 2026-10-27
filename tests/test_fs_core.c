#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../kernel/fs_core.h"
#define SECTORS 8
static uint8_t disk[SECTORS*512];
static int rd(void *ctx,uint64_t lba,uint32_t n,void *out){uint8_t *d=ctx;if(lba>=SECTORS||n>SECTORS-lba)return -1;memcpy(out,d+lba*512,n*512);return BLOCK_OK;}
static void w16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void w32(uint8_t*p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t header_hash(uint8_t*p){uint8_t c[36];memcpy(c,p,36);memset(c+32,0,4);return fs_checksum(c,36);}
static void make_disk(void){memset(disk,0,sizeof disk);uint8_t*h=disk;memcpy(h,"AOSFS1\0\0",8);w16(h+8,1);w16(h+10,512);w32(h+12,36);w32(h+16,SECTORS);w32(h+20,1);w16(h+24,1);w16(h+26,64);w32(h+28,2);w32(h+32,header_hash(h));uint8_t*e=disk+512;memcpy(e,"hello.elf",10);w32(e+32,1);w32(e+36,2);w32(e+40,5);w32(e+44,1);memcpy(disk+1024,"hello",5);w32(e+48,fs_checksum(disk+1024,5));}
int main(void){block_device_t d={disk,rd,SECTORS,512};fs_volume_t v={0};uint8_t out[8];size_t n=0;make_disk();assert(fs_mount(&v,&d)==FS_OK);assert(fs_file_count(&v)==1);assert(fs_lookup(&v,"hello.elf")!=0);assert(fs_read_file(&v,"hello.elf",out,sizeof out,&n)==FS_OK&&n==5&&!memcmp(out,"hello",5));assert(fs_read_file(&v,"absent",out,sizeof out,&n)==FS_NOT_FOUND);assert(fs_read_file(&v,"hello.elf",out,4,&n)==FS_BUFFER_TOO_SMALL);
make_disk();disk[0]='X';assert(fs_mount(&v,&d)==FS_CORRUPT);
make_disk();w32(disk+512+36,SECTORS-1);w32(disk+512+44,2);assert(fs_mount(&v,&d)==FS_CORRUPT);
make_disk();disk[1024]^=1;assert(fs_mount(&v,&d)==FS_OK);assert(fs_read_file(&v,"hello.elf",out,sizeof out,&n)==FS_CORRUPT);
make_disk();disk[1024]^=1;assert(fs_mount(&v,&d)==FS_OK);assert(fs_read_file(&v,"hello.elf",out,sizeof out,&n)==FS_CORRUPT);
make_disk();disk[40]=1;assert(fs_mount(&v,&d)==FS_CORRUPT);
make_disk();disk[512]='.';disk[513]='.';disk[514]='/';assert(fs_mount(&v,&d)==FS_CORRUPT);
make_disk();w16(disk+24,2);w32(disk+32,0);w32(disk+32,header_hash(disk));memcpy(disk+512+64,"hello.elf\0",10);w32(disk+512+64+32,1);w32(disk+512+64+36,3);w32(disk+512+64+40,1);w32(disk+512+64+44,1);assert(fs_mount(&v,&d)==FS_CORRUPT);
make_disk();w16(disk+24,2);w32(disk+32,0);w32(disk+32,header_hash(disk));memcpy(disk+512+64,"other\0",6);w32(disk+512+64+32,1);w32(disk+512+64+36,2);w32(disk+512+64+40,1);w32(disk+512+64+44,1);assert(fs_mount(&v,&d)==FS_CORRUPT);
make_disk();w32(disk+512+32,2);assert(fs_mount(&v,&d)==FS_CORRUPT);
make_disk();d.sector_count=1;assert(fs_mount(&v,&d)==FS_INVALID);
return 0;}
