#include <stddef.h>
#include <stdint.h>
#include "ahci.h"
#include "pmm.h"
#include "vm.h"
#include "sync.h"
#include "../drivers/serial.h"
#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC
#define HBA_PORT_OFFSET 0x100u
#define HBA_PORT_STRIDE 0x80u
#define AHCI_TIMEOUT 50000000u
#define ATA_IDENTIFY 0xECu
#define ATA_READ_DMA_EXT 0x25u
typedef struct __attribute__((packed,aligned(16))) { uint16_t flags,prdtl; volatile uint32_t prdbc; uint32_t ctba,ctbau,reserved[4]; } cmd_header_t;
typedef struct __attribute__((packed)) { uint32_t dba,dbau,reserved,dbc; } prdt_t;
typedef struct __attribute__((packed,aligned(128))) { uint8_t cfis[64]; uint8_t acmd[16]; uint8_t reserved[48]; prdt_t prdt[1]; } cmd_table_t;
static volatile uint8_t *hba;
static volatile uint8_t *port;

static uint64_t disk_sectors;
static uint32_t cmd_list_pa, fis_pa, table_pa;
static block_device_t *active_device;
static sync_lock_t io_lock=SYNC_LOCK_INIT(1);
static cmd_header_t command_list[32] __attribute__((aligned(1024)));
static uint8_t received_fis[256] __attribute__((aligned(256)));
static cmd_table_t command_table __attribute__((aligned(128)));
static uint8_t dma_buffer[512] __attribute__((aligned(4096)));
static uint16_t identify_data[256] __attribute__((aligned(4096)));
static void ahci_log_hex(serial_level_t level,const char *label,uint64_t value);
static void ahci_log_port_registers(serial_level_t level,const char *reason);
static int aosfs_signature(void){static const uint8_t magic[8]={'A','O','S','F','S','1',0,0};for(unsigned i=0;i<8;++i)if(dma_buffer[i]!=magic[i])return 0;return 1;}static uint32_t pci_address(uint8_t dev,uint8_t fn,uint8_t off){return 0x80000000u|((uint32_t)dev<<11)|((uint32_t)fn<<8)|(off&0xFCu);}
static uint32_t pci_read(uint8_t dev,uint8_t fn,uint8_t off){__asm__ volatile("outl %0,%1"::"a"(pci_address(dev,fn,off)),"d"((uint16_t)PCI_ADDR));uint32_t v;__asm__ volatile("inl %1,%0":"=a"(v):"d"((uint16_t)PCI_DATA));return v;}
static void pci_write(uint8_t dev,uint8_t fn,uint8_t off,uint32_t value){__asm__ volatile("outl %0,%1"::"a"(pci_address(dev,fn,off)),"d"((uint16_t)PCI_ADDR));__asm__ volatile("outl %0,%1"::"a"(value),"d"((uint16_t)PCI_DATA));}
static int log_mmio_alias(uint64_t physical_page){
    uint64_t alias=0;uint32_t flags=0;int status=vm_find_physical_alias(physical_page,&alias,&flags);
    ahci_log_hex(SERIAL_INFO,"AHCI MMIO physical-alias scan status=",(uint64_t)(int64_t)status);
    if(status==VM_OK){ahci_log_hex(SERIAL_INFO,"AHCI MMIO physical page already aliased at VA=",alias);ahci_log_hex(SERIAL_INFO,"AHCI MMIO alias effective flags=",flags);return 1;}
    if(status==VM_NOT_MAPPED)return 0;
    serial_log(SERIAL_ERROR,"AHCI MMIO physical-alias scan failed; refusing to create another mapping.");return -1;
}
static int map_identity_page(uint64_t address){
    uint64_t va=address&~UINT64_C(4095),pa=0;uint32_t flags=0;
    int before=vm_translate_address(va,&pa,&flags);
    ahci_log_hex(SERIAL_INFO,"AHCI MMIO pre-map translate status=",(uint64_t)(int64_t)before);
    if(before==VM_OK){
        ahci_log_hex(SERIAL_INFO,"AHCI MMIO pre-map translated PA=",pa);
        ahci_log_hex(SERIAL_INFO,"AHCI MMIO pre-map effective flags=",flags);
        if(pa!=va){serial_log(SERIAL_ERROR,"AHCI MMIO identity VA already maps a different physical page; refusing remap.");(void)log_mmio_alias(va);return 0;}
        if(!(flags&VM_MAP_WRITE)){serial_log(SERIAL_ERROR,"AHCI MMIO identity mapping is not effectively writable.");return 0;}
        if((flags&(VM_MAP_PCD|VM_MAP_PWT))!=(VM_MAP_PCD|VM_MAP_PWT))
            serial_log(SERIAL_WARN,"AHCI MMIO identity mapping is cacheable; setting PCD|PWT on the 4 KiB BAR page.");
        int cache_status=vm_set_page_cache_flags(va,VM_MAP_PCD|VM_MAP_PWT);
        ahci_log_hex(SERIAL_INFO,"AHCI MMIO uncached policy/update status=",(uint64_t)(int64_t)cache_status);
        if(cache_status!=VM_OK){serial_log(SERIAL_ERROR,"AHCI MMIO could not establish a verified UC cache policy; no registers will be accessed.");return 0;}
        pa=0;flags=0;int after=vm_translate_address(va,&pa,&flags);
        ahci_log_hex(SERIAL_INFO,"AHCI MMIO post-cache-update translate status=",(uint64_t)(int64_t)after);
        if(after==VM_OK){ahci_log_hex(SERIAL_INFO,"AHCI MMIO post-cache-update physical address=",pa);ahci_log_hex(SERIAL_INFO,"AHCI MMIO post-cache-update effective flags=",flags);}
        if(after!=VM_OK||pa!=va||!(flags&VM_MAP_WRITE)||
           (flags&(VM_MAP_PCD|VM_MAP_PWT))!=(VM_MAP_PCD|VM_MAP_PWT)){
            serial_log(SERIAL_ERROR,"AHCI MMIO cache update did not produce an identity writable PCD|PWT mapping; no registers will be accessed.");return 0;
        }
        return 1;
    }
    if(before!=VM_NOT_MAPPED){serial_log(SERIAL_ERROR,"AHCI MMIO pre-map translation returned an unexpected VM error.");return 0;}
    int alias_status=log_mmio_alias(va);
    if(alias_status!=0){if(alias_status>0)serial_log(SERIAL_ERROR,"AHCI MMIO physical page is mapped at another VA; refusing a cache-alias mapping.");return 0;}
    serial_log(SERIAL_INFO,"AHCI MMIO identity VA is absent; attempting supervisor writable PCD|PWT mapping.");
    int map_status=vm_map_page(va,va,VM_MAP_WRITE|VM_MAP_PCD|VM_MAP_PWT);
    ahci_log_hex(SERIAL_INFO,"AHCI MMIO vm_map_page status=",(uint64_t)(int64_t)map_status);
    pa=0;flags=0;int after=vm_translate_address(va,&pa,&flags);
    ahci_log_hex(SERIAL_INFO,"AHCI MMIO post-map translate status=",(uint64_t)(int64_t)after);
    if(after==VM_OK){ahci_log_hex(SERIAL_INFO,"AHCI MMIO post-map translated PA=",pa);ahci_log_hex(SERIAL_INFO,"AHCI MMIO post-map effective flags=",flags);}
    if(map_status!=VM_OK){serial_log(SERIAL_ERROR,"AHCI MMIO mapping API failed; no AHCI registers will be accessed.");return 0;}
    if(after!=VM_OK||pa!=va||!(flags&VM_MAP_WRITE)||
       (flags&(VM_MAP_PCD|VM_MAP_PWT))!=(VM_MAP_PCD|VM_MAP_PWT)){
        serial_log(SERIAL_ERROR,"AHCI MMIO post-map translation failed identity/permission/cache validation; no registers will be accessed.");return 0;
    }
    serial_log(SERIAL_INFO,"AHCI MMIO identity mapping validated writable and PCD|PWT.");return 1;
}
static int dma_address(const void *p,uint32_t *out){uint64_t pa;uint32_t flags;if(!p||!out||vm_translate_address((uint64_t)(uintptr_t)p,&pa,&flags)!=VM_OK||pa>UINT32_MAX)return 0;*out=(uint32_t)pa;return 1;}
static void mmio_write(uint32_t off,uint32_t value){*(volatile uint32_t *)(hba+off)=value;}
static uint32_t mmio_read(uint32_t off){return *(volatile uint32_t *)(hba+off);}
static void port_write(uint32_t off,uint32_t value){*(volatile uint32_t *)(port+off)=value;}
static uint32_t port_read(uint32_t off){return *(volatile uint32_t *)(port+off);}
static int wait_clear(uint32_t off,uint32_t mask){for(uint32_t i=0;i<AHCI_TIMEOUT;++i){if(!(port_read(off)&mask))return 1;__asm__ volatile("pause");}return 0;}
static int stop_engine(void){uint32_t c=port_read(0x18);port_write(0x18,c&~(1u<<0));if(!wait_clear(0x18,1u<<15))return 0;c=port_read(0x18);port_write(0x18,c&~(1u<<4));return wait_clear(0x18,1u<<14)&&wait_clear(0x38,UINT32_MAX)&&wait_clear(0x34,UINT32_MAX);}
static int start_engine(void){
    uint32_t c=port_read(0x18);port_write(0x18,c|(1u<<4));
    uint32_t i=0;for(;i<AHCI_TIMEOUT;++i){if(port_read(0x18)&(1u<<14))break;__asm__ volatile("pause");}
    if(i==AHCI_TIMEOUT)return 0;
    port_write(0x18,port_read(0x18)|(1u<<0));
    return 1;
}
static int issue(uint8_t command,uint64_t lba,uint16_t sectors,uint8_t *buffer){
    uint32_t pa;
    if(!buffer||!dma_address(buffer,&pa)||pa>UINT32_MAX-512)return 0;
    uint32_t tfd=port_read(0x20);
    if((tfd&0x88u)||!wait_clear(0x20,0x88u))return 0;
    port_write(0x10,0xFFFFFFFFu);
    cmd_header_t *ch=&command_list[0];ch->flags=5;ch->prdtl=1;ch->prdbc=0;ch->ctba=table_pa;ch->ctbau=0;
    for(unsigned i=0;i<sizeof(command_table);i++)((uint8_t *)&command_table)[i]=0;
    command_table.prdt[0].dba=pa;command_table.prdt[0].dbau=0;command_table.prdt[0].dbc=(uint32_t)sectors*512u-1u;
    uint8_t *f=command_table.cfis;f[0]=0x27;f[1]=0x80;f[2]=command;
    f[4]=(uint8_t)lba;f[5]=(uint8_t)(lba>>8);f[6]=(uint8_t)(lba>>16);f[7]=0x40;
    f[8]=(uint8_t)(lba>>24);f[9]=(uint8_t)(lba>>32);f[10]=(uint8_t)(lba>>40);
    f[12]=(uint8_t)sectors;f[13]=(uint8_t)(sectors>>8);
    if(command==ATA_IDENTIFY){f[2]=ATA_IDENTIFY;f[4]=f[5]=f[6]=f[7]=f[8]=f[9]=f[10]=f[12]=f[13]=0;}
    __asm__ volatile("mfence" ::: "memory");port_write(0x38,1);
    for(uint32_t i=0;i<AHCI_TIMEOUT;++i){if(!(port_read(0x38)&1u)){__asm__ volatile("mfence" ::: "memory");if(port_read(0x10)&(1u<<30))return 0;return (port_read(0x20)&1u)==0&&ch->prdbc==(uint32_t)sectors*512u;}__asm__ volatile("pause");}
    return 0;
}
static int read_callback(void *ctx,uint64_t lba,uint32_t count,void *buffer){(void)ctx;if(!active_device||lba>=disk_sectors||count>disk_sectors-lba)return BLOCK_BOUNDS;sync_guard_t guard={0};if(sync_lock_acquire(&io_lock,&guard)!=SYNC_OK){serial_log(SERIAL_ERROR,"AHCI storage lock-order or recursion violation rejected read.");return BLOCK_IO_ERROR;}uint8_t *dst=buffer;int ok=1;for(uint32_t i=0;i<count;++i){if(!issue(ATA_READ_DMA_EXT,lba+i,1,dma_buffer)){ok=0;break;}for(unsigned j=0;j<512;++j)dst[(size_t)i*512+j]=dma_buffer[j];}sync_lock_release(&io_lock,&guard);if(!ok)ahci_log_port_registers(SERIAL_ERROR,"AHCI block read failed; see command status registers.");return ok?BLOCK_OK:BLOCK_IO_ERROR;}
static int probe_port(volatile uint8_t *p){uint32_t ssts=*(volatile uint32_t *)(p+0x28),sig=*(volatile uint32_t *)(p+0x24);return (ssts&0x0Fu)==3&&((ssts>>8)&0x0Fu)==1&&sig==0x00000101u;}
static void ahci_log_hex(serial_level_t level,const char *label,uint64_t value){
    static const char digits[]="0123456789ABCDEF";char message[112];size_t n=0;
    while(label[n]&&n<64){message[n]=label[n];++n;}message[n++]='0';message[n++]='x';
    for(int shift=60;shift>=0;shift-=4){message[n++]=digits[(value>>shift)&15u];}
    message[n]=0;serial_log(level,message);
}
static void ahci_log_port_registers(serial_level_t level,const char *reason){
    serial_log(level,reason);ahci_log_hex(level,"AHCI port SSTS=",port_read(0x28));
    ahci_log_hex(level,"AHCI port SIG=",port_read(0x24));ahci_log_hex(level,"AHCI port TFD=",port_read(0x20));
    ahci_log_hex(level,"AHCI port IS=",port_read(0x10));ahci_log_hex(level,"AHCI port CI=",port_read(0x38));
    ahci_log_hex(level,"AHCI port CMD=",port_read(0x18));ahci_log_hex(level,"AHCI port SACT=",port_read(0x34));
    ahci_log_hex(level,"AHCI command PRDBC=",command_list[0].prdbc);
}
static int validate_dma_object(const char *name,const void *pointer,uint32_t alignment,uint32_t length,uint32_t *physical){
    uint64_t va=(uint64_t)(uintptr_t)pointer;
    if(!dma_address(pointer,physical)){ahci_log_hex(SERIAL_ERROR,name,va);serial_log(SERIAL_ERROR,"AHCI DMA address is unmapped or exceeds 32-bit range.");return 0;}
    ahci_log_hex(SERIAL_DEBUG,name,*physical);
    if((alignment&&(*physical&(alignment-1u)))||*physical>UINT32_MAX-length){serial_log(SERIAL_ERROR,"AHCI DMA address alignment/bounds validation failed.");return 0;}
    return 1;
}
int ahci_init(block_device_t *device){
    if(!device){serial_log(SERIAL_ERROR,"AHCI init rejected null block device.");return 0;}
    uint8_t found_dev=0,found_fn=0;uint32_t bar=0,id=0;int found=0;
    for(uint8_t d=0;d<32&&!found;++d){
        uint32_t id0=pci_read(d,0,0);if((id0&0xFFFFu)==0xFFFFu)continue;
        uint8_t funcs=(pci_read(d,0,0x0C)&0x00800000u)?8:1;
        for(uint8_t f=0;f<funcs;++f){
            uint32_t cc=pci_read(d,f,8);
            if(((cc>>24)&0xFFu)==1&&((cc>>16)&0xFFu)==6&&((cc>>8)&0xFFu)==1){
                found_dev=d;found_fn=f;id=pci_read(d,f,0);bar=pci_read(d,f,0x24);found=1;break;
            }
        }
    }
    if(!found){serial_log(SERIAL_ERROR,"AHCI PCI discovery failed: no class 01/subclass 06/prog-if 01 controller on bus 0.");return 0;}
    serial_log(SERIAL_INFO,"AHCI PCI controller found by class-code scan.");
    ahci_log_hex(SERIAL_INFO,"AHCI PCI BDF=",((uint64_t)found_dev<<8)|found_fn);
    ahci_log_hex(SERIAL_INFO,"AHCI PCI vendor/device=",id);
    ahci_log_hex(SERIAL_INFO,"AHCI BAR5=",bar);
    if((bar&1u)||((bar>>1)&3u)==2){serial_log(SERIAL_ERROR,"AHCI BAR5 rejected: expected a 32-bit memory BAR.");return 0;}
    serial_log(SERIAL_INFO,"AHCI BAR5 validated as a 32-bit memory BAR.");
    uint64_t mmio=(uint64_t)(bar&0xFFFFFFF0u);
    if(!mmio||(mmio&0xFFFu)||mmio>UINT32_MAX-0x2000){serial_log(SERIAL_ERROR,"AHCI BAR5 rejected: zero, unaligned, or outside supported MMIO bounds.");return 0;}
    uint32_t cmd=pci_read(found_dev,found_fn,4);pci_write(found_dev,found_fn,4,(cmd&0xFFFFu)|6u);
    if(!map_identity_page(mmio)){ahci_log_hex(SERIAL_ERROR,"AHCI MMIO first page mapping failed VA=",mmio);return 0;}
    if(!map_identity_page(mmio+0x1000)){ahci_log_hex(SERIAL_ERROR,"AHCI MMIO second page mapping failed VA=",mmio+0x1000);return 0;}
    hba=(volatile uint8_t *)(uintptr_t)mmio;serial_log(SERIAL_INFO,"AHCI MMIO pages mapped identity, writable, and uncached.");
    uint32_t cap=mmio_read(0),cap2=mmio_read(0x24);ahci_log_hex(SERIAL_INFO,"AHCI CAP=",cap);ahci_log_hex(SERIAL_INFO,"AHCI CAP2=",cap2);
    if(cap2&1u){
        uint32_t bohc=mmio_read(0x28);ahci_log_hex(SERIAL_INFO,"AHCI BOHC before handoff=",bohc);mmio_write(0x28,bohc|(1u<<1));
        uint32_t i=0;for(;i<AHCI_TIMEOUT;++i){bohc=mmio_read(0x28);if(!(bohc&((1u<<0)|(1u<<4))))break;__asm__ volatile("pause");}
        if(i==AHCI_TIMEOUT){ahci_log_hex(SERIAL_ERROR,"AHCI BIOS/OS handoff timeout BOHC=",bohc);return 0;}
        ahci_log_hex(SERIAL_INFO,"AHCI BOHC after handoff=",bohc);
    }else serial_log(SERIAL_INFO,"AHCI BIOS/OS handoff not required by CAP2.");
    uint32_t ghc=mmio_read(4);mmio_write(4,(ghc|(1u<<31))&~(1u<<1));mmio_write(8,0xFFFFFFFFu);
    uint32_t pi=mmio_read(0x0C);ahci_log_hex(SERIAL_INFO,"AHCI implemented ports PI=",pi);
    if(!pi){serial_log(SERIAL_ERROR,"AHCI controller reports no implemented ports.");return 0;}
    uint32_t clb,fb,ct,dma,idpa;
    if(!validate_dma_object("AHCI command-list DMA address=",command_list,1024,1024,&clb)||
       !validate_dma_object("AHCI received-FIS DMA address=",received_fis,256,256,&fb)||
       !validate_dma_object("AHCI command-table DMA address=",&command_table,128,(uint32_t)sizeof(command_table),&ct)||
       !validate_dma_object("AHCI bounce-buffer DMA address=",dma_buffer,512,512,&dma)||
       !validate_dma_object("AHCI IDENTIFY-buffer DMA address=",identify_data,512,512,&idpa)){
        serial_log(SERIAL_ERROR,"AHCI DMA-address validation failed before touching ports.");return 0;
    }
    disk_sectors=0;active_device=0;
    for(uint8_t i=0;i<32;++i){
        if(!(pi&(1u<<i)))continue;
        volatile uint8_t *p=hba+HBA_PORT_OFFSET+(uint32_t)i*HBA_PORT_STRIDE;port=p;
        uint32_t ssts=port_read(0x28),sig=port_read(0x24);
        ahci_log_hex(SERIAL_DEBUG,"AHCI implemented port index=",i);
        ahci_log_hex(SERIAL_INFO,"AHCI port SSTS=",ssts);ahci_log_hex(SERIAL_INFO,"AHCI port SIG=",sig);
        if(!probe_port(p)){
            serial_log(SERIAL_WARN,"AHCI port skipped: link is not active SATA ATA (DET/IPM/signature mismatch).");continue;
        }
        if(!stop_engine()){ahci_log_port_registers(SERIAL_ERROR,"AHCI port engine stop failed.");continue;}
        ahci_log_hex(SERIAL_INFO,"AHCI port engine stopped CMD=",port_read(0x18));
        for(unsigned x=0;x<1024;++x)((uint8_t *)command_list)[x]=0;
        for(unsigned x=0;x<256;++x)received_fis[x]=0;
        cmd_list_pa=clb;fis_pa=fb;table_pa=ct;
        *(volatile uint32_t *)(p+0x00)=cmd_list_pa;*(volatile uint32_t *)(p+0x04)=0;
        *(volatile uint32_t *)(p+0x08)=fis_pa;*(volatile uint32_t *)(p+0x0C)=0;
        *(volatile uint32_t *)(p+0x10)=0xFFFFFFFFu;*(volatile uint32_t *)(p+0x30)=0xFFFFFFFFu;
        *(volatile uint32_t *)(p+0x14)=0;__asm__ volatile("mfence":::"memory");
        if(!start_engine()){ahci_log_port_registers(SERIAL_ERROR,"AHCI port command/FIS engine start failed.");continue;}
        ahci_log_hex(SERIAL_INFO,"AHCI port engine started CMD=",port_read(0x18));
        for(unsigned j=0;j<256;++j)identify_data[j]=0;
        if(!issue(ATA_IDENTIFY,0,1,(uint8_t *)identify_data)){ahci_log_port_registers(SERIAL_ERROR,"ATA IDENTIFY failed; see port command registers above.");continue;}
        serial_log(SERIAL_INFO,"ATA IDENTIFY command completed successfully.");
        uint16_t w83=identify_data[83];
        disk_sectors=(uint64_t)identify_data[100]|((uint64_t)identify_data[101]<<16)|((uint64_t)identify_data[102]<<32)|((uint64_t)identify_data[103]<<48);
        ahci_log_hex(SERIAL_INFO,"ATA IDENTIFY word 83=",w83);ahci_log_hex(SERIAL_INFO,"ATA capacity sectors=",disk_sectors);
        if(!disk_sectors||(w83&0xC000u)!=0x4000u||!(w83&(1u<<10))){serial_log(SERIAL_ERROR,"ATA disk rejected: invalid capacity or LBA48 not advertised.");continue;}
        uint16_t sector_info=identify_data[106];
        if((sector_info&0xC000u)==0x4000u&&(sector_info&(1u<<12))){
            uint32_t words=(uint32_t)identify_data[117]|((uint32_t)identify_data[118]<<16);
            ahci_log_hex(SERIAL_INFO,"ATA logical-sector words=",words);
            if(words!=256u){serial_log(SERIAL_ERROR,"ATA disk rejected: logical sector size is not 512 bytes.");continue;}
        }
        active_device=device;device->context=0;device->read=read_callback;device->sector_count=disk_sectors;device->sector_size=512;
        if(!issue(ATA_READ_DMA_EXT,0,1,dma_buffer)){ahci_log_port_registers(SERIAL_ERROR,"AOSFS signature sector read command failed.");active_device=0;continue;}
        uint64_t signature=0;for(unsigned k=0;k<8;++k)signature|=(uint64_t)dma_buffer[k]<<(k*8);
        ahci_log_hex(SERIAL_INFO,"AOSFS LBA0 signature read=",signature);
        if(!aosfs_signature()){serial_log(SERIAL_WARN,"SATA disk is readable but does not contain the AOSFS v1 signature at LBA 0.");active_device=0;continue;}
        serial_log(SERIAL_INFO,"AHCI selected SATA disk by AOSFS signature at LBA 0.");return 1;
    }
    serial_log(SERIAL_ERROR,"AHCI scan completed without a readable AOSFS data disk.");return 0;
}
int storage_mount_aosfs(block_device_t *device,fs_volume_t *volume){
    if(!device||!volume){serial_log(SERIAL_ERROR,"AOSFS mount rejected null argument.");return 0;}
    if(!ahci_init(device)){serial_log(SERIAL_ERROR,"AOSFS mount not attempted because AHCI initialization failed; inspect AHCI diagnostics above.");return 0;}
    int status=fs_mount(volume,device);ahci_log_hex(status==FS_OK?SERIAL_INFO:SERIAL_ERROR,"AOSFS fs_mount status=",(uint64_t)(int64_t)status);
    return status==FS_OK;
}