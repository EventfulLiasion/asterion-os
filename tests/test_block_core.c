#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../kernel/block_core.h"
static int backend(void *ctx,uint64_t lba,uint32_t count,void *out){(void)ctx;(void)lba;(void)count;(void)out;return BLOCK_OK;}
static int fail_backend(void *ctx,uint64_t lba,uint32_t count,void *out){(void)ctx;(void)lba;(void)count;(void)out;return -9;}
int main(void){block_device_t d={0,backend,16,512};uint8_t buf[512];assert(block_read(&d,0,1,buf)==BLOCK_OK);assert(block_read(&d,16,1,buf)==BLOCK_BOUNDS);assert(block_read(&d,15,2,buf)==BLOCK_BOUNDS);assert(block_read(&d,0,0,buf)==BLOCK_INVALID);assert(block_read(&d,0,1,0)==BLOCK_INVALID);assert(block_read(&d,0,129,buf)==BLOCK_INVALID);d.read=fail_backend;assert(block_read(&d,0,1,buf)==BLOCK_IO_ERROR);d.read=0;assert(block_read(&d,0,1,buf)==BLOCK_INVALID);return 0;}
