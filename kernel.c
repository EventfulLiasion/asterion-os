/*
 * Asterion OS — a freestanding x86_64 UEFI kernel experiment.
 * The UEFI application entry is only the boot handoff. The kernel takes the
 * GOP framebuffer, reads the firmware memory map, exits boot services, and
 * thereafter draws its own terminal and polls the i8042 keyboard directly.
 */
#include <stdint.h>
#include <stddef.h>
#include "include/boot_info.h"
#include "drivers/serial.h"
#include "include/boot_validate.h"
#include "kernel/calc.h"
#include "kernel/api.h"
#include "drivers/keyboard.h"
#include "drivers/framebuffer.h"
#include "arch/x86_64/interrupts.h"
#include "kernel/pmm.h"
#include "kernel/vm.h"
#include "kernel/heap.h"
#include "kernel/ahci.h"
#include "kernel/fs_core.h"
#include "kernel/threads.h"
#include "kernel/process.h"

static char input_line[120];
static uint32_t input_len;
static int shift_down, caps_lock;
static uint64_t boot_tsc;
static boot_info_t boot_info;
volatile uint64_t asterion_demo_progress[2];
void asterion_demo_worker(void *argument) {
    uint32_t index=(uint32_t)(uintptr_t)argument;
    for(uint64_t i=0;i<UINT64_C(20000000);++i) asterion_demo_progress[index]=i+1;
}
/* Forces a PE DIR64 relocation; the handoff checks it after firmware loading. */
static boot_info_t * volatile relocation_anchor = &boot_info;

static void line_add(const char *message) { console_log(SERIAL_INFO, message); }

static inline void out8(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t in8(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void out32(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t in32(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint64_t rdtsc(void) { uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((uint64_t)hi<<32)|lo; }
static int str_eq(const char *a,const char *b) { while(*a&&*b&&*a==*b){a++;b++;} return *a==*b; }
static char upper(char c) { return c>='a'&&c<='z'?(char)(c-'a'+'A'):c; }
static int str_eq_ci(const char *a,const char *b) { while(*a&&*b&&upper(*a)==upper(*b)){a++;b++;} return *a==*b; }
static const char *skip_spaces(const char *s) { while(*s==' '||*s=='\t')s++;return s; }
static void copystr(char *d,const char *s,size_t n) { size_t i=0; while(i+1<n&&s[i]){d[i]=s[i];i++;}d[i]=0; }

static void number(char *out,uint64_t n) { char t[24];int k=0;if(!n){out[0]='0';out[1]=0;return;}while(n&&k<23){t[k++]=(char)('0'+n%10);n/=10;}int j=0;while(k)out[j++]=t[--k];out[j]=0; }
static void append(char *dst,const char *src) { size_t i=0;while(i<119&&dst[i])i++;while(*src&&i<119)dst[i++]=*src++;dst[i]=0; }
static void hexnum(char *out,uint64_t n) { static const char h[]="0123456789ABCDEF";out[0]='0';out[1]='X';for(int i=0;i<16;i++)out[2+i]=h[(n>>(60-i*4))&15];out[18]=0; }
static fs_volume_t rootfs;
static int rootfs_mounted;

static void command_help(void) {
    line_add("COMMANDS  -------------------------------------------------");
    line_add(" HELP       Show this command list");line_add(" ABOUT      Kernel and architecture details");
    line_add(" CLEAR      Clear the console       COLOR [G|A|B]");
    line_add(" MEM        Usable physical memory   MAP");
    line_add(" SCREEN     Framebuffer metadata       CPU");
    line_add(" PCI        Scan PCI bus 0           FS         List read-only disk files");
    line_add(" UPTIME     Show kernel uptime");
    line_add(" VM         Paging levels and NX status");
    line_add(" HEAP       Kernel heap capacity and usage");
    line_add(" THREADS    List kernel threads       SCHEDTEST Run scheduler demo");
    line_add(" ECHO text  Print text              CALC 12*4");
    line_add(" LS         List built-in files      CAT README");
    line_add(" REBOOT     Restart the machine     HALT");
    line_add(" KEYS       Keyboard scan codes      HELP");
}
static void cpu_info(void) {
    char msg[120]="CPU vendor: ";append(msg,boot_info.cpu_vendor);line_add(msg);
    if(boot_info.cpu_brand[0]){msg[0]=0;append(msg,"CPU model: ");append(msg,boot_info.cpu_brand);line_add(msg);}
    char x[24];hexnum(x,boot_info.cpu_signature);msg[0]=0;append(msg,"CPUID signature: ");append(msg,x+14);line_add(msg);
    msg[0]=0;append(msg,"Features: ");append(msg,(boot_info.cpu_feature_edx&(1u<<9))?"APIC ":"no-APIC ");append(msg,(boot_info.cpu_feature_edx&(1u<<4))?"TSC ":"no-TSC ");append(msg,(boot_info.cpu_feature_edx&(1u<<26))?"SSE2 ":"no-SSE2 ");append(msg,(boot_info.cpu_extended_feature_edx&(1u<<20))?"NX":"no-NX");line_add(msg);
}
static void memory_map_command(void) {
    char msg[120]="UEFI descriptors: ",n[24];number(n,boot_info.memory_descriptor_count);append(msg,n);append(msg," | descriptor bytes: ");number(n,boot_info.memory_descriptor_size);append(msg,n);line_add(msg);
    uint64_t shown=0;
    for(uint64_t i=0;i<boot_info.memory_descriptor_count&&shown<8;i++){
        const EFI_MEMORY_DESCRIPTOR *d=(const EFI_MEMORY_DESCRIPTOR*)((const uint8_t*)boot_info.memory_map+i*boot_info.memory_descriptor_size);
        if(d->type!=EFI_MEMORY_CONVENTIONAL)continue;
        char base[24],pages[24];hexnum(base,d->physical_start);number(pages,d->pages);msg[0]=0;append(msg,"usable base ");append(msg,base);append(msg," pages ");append(msg,pages);line_add(msg);shown++;
    }
}


static void pci_scan(void) {
    uint32_t found=0;line_add("PCI scan: bus 0 (configuration mechanism #1)");
    for(uint32_t slot=0;slot<32;slot++)for(uint32_t fn=0;fn<8;fn++){
        uint32_t addr=0x80000000u|(slot<<11)|(fn<<8);out32(0xCF8,addr);uint32_t id=in32(0xCFC);
        uint16_t ven=(uint16_t)id,dev=(uint16_t)(id>>16);if(ven==0xFFFF)continue;
        char msg[120]="  PCI ";char n[24];number(n,slot);append(msg,n);append(msg,".");number(n,fn);append(msg,n);append(msg,"  vendor=");hexnum(n,ven);append(msg,n+14);append(msg," device=");hexnum(n,dev);append(msg,n+14);line_add(msg);found++;
        if(fn==0){out32(0xCF8,addr+4);uint32_t h=in32(0xCFC);if(!(h&(1u<<23)))break;}
    }
    char msg[120]="PCI functions found: ",n[24];number(n,found);append(msg,n);line_add(msg);
}
static void calc_command(const char *expr) {
    int64_t value;
    calc_status_t status=calc_evaluate(expr,&value);
    if(status==CALC_INVALID_EXPRESSION){line_add("Usage: CALC [-]integer [+|-|*|/] [-]integer");return;}
    if(status==CALC_DIVIDE_BY_ZERO){line_add("CALC error: division by zero");return;}
    if(status==CALC_OVERFLOW){line_add("CALC error: signed 64-bit overflow");return;}
    char out[120]="= ";
    uint64_t magnitude=(uint64_t)value;
    if(value<0){append(out,"-");magnitude=UINT64_C(0)-magnitude;}
    char n[24];number(n,magnitude);append(out,n);line_add(out);
}
static void run_command(char *cmd) {
    char token[16];size_t length=0;
    const char *p=skip_spaces(cmd);
    if(!*p)return;
    while(p[length]&&p[length]!=' '&&p[length]!='\t'){
        if(length+1>=sizeof(token)){line_add("Command name is too long.");return;}
        token[length]=upper(p[length]);length++;
    }
    token[length]=0;
    const char *args=skip_spaces(p+length);
    int no_args=!*args;
    if(str_eq(token,"HELP")&&no_args){command_help();return;}
    if((str_eq(token,"CLEAR")||str_eq(token,"CLS"))&&no_args){console_clear();return;}
    if(str_eq(token,"ABOUT")&&no_args){line_add("ASTERION OS / FREESTANDING UEFI KERNEL");line_add("x86_64  |  C17  |  GOP framebuffer  |  i8042 polling");line_add("Boot Services have been exited. No host OS is running.");line_add("Firmware supplied the UEFI loader and initial memory map.");return;}
    if(str_eq(token,"CPU")&&no_args){cpu_info();return;}
    if(str_eq(token,"PCI")&&no_args){pci_scan();return;}
    if(str_eq(token,"FS")&&no_args){if(!rootfs_mounted){line_add("FS unavailable: no supported AOSFS disk mounted.");return;}line_add("AOSFS read-only files:");for(uint32_t i=0;i<fs_file_count(&rootfs);++i){const fs_file_t *f=fs_file_at(&rootfs,i);char msg[100],n[24];msg[0]=0;append(msg,"  ");append(msg,f->name);append(msg,"  ");number(n,f->byte_size);append(msg,n);append(msg," bytes");line_add(msg);}return;}
    if(str_eq(token,"MEM")&&no_args){pmm_stats_t s;pmm_get_stats(&s);char msg[120]="PMM free: ",n[24];number(n,s.free_pages*PMM_PAGE_SIZE/(1024*1024));append(msg,n);append(msg," MiB / ");number(n,s.free_pages);append(msg,n);append(msg," frames");line_add(msg);msg[0]=0;append(msg,"Allocated: ");number(n,s.allocated_pages);append(msg,n);append(msg," | reserved conventional: ");number(n,s.reserved_conventional_pages);append(msg,n);line_add(msg);msg[0]=0;append(msg,"UEFI conventional: ");number(n,s.conventional_pages*PMM_PAGE_SIZE/(1024*1024));append(msg,n);append(msg," MiB; managed cap ");number(n,PMM_MAX_PHYSICAL_ADDRESS/(1024*1024*1024));append(msg,n);append(msg," GiB");line_add(msg);return;}
    if(str_eq(token,"MAP")&&no_args){memory_map_command();return;}
    if(str_eq(token,"SCREEN")&&no_args){char msg[120]="Framebuffer: ",n[24];number(n,boot_info.framebuffer_width);append(msg,n);append(msg,"x");number(n,boot_info.framebuffer_height);append(msg,n);append(msg," stride ");number(n,boot_info.framebuffer_stride);append(msg,n);append(msg," format ");number(n,boot_info.pixel_format);append(msg,n);line_add(msg);msg[0]=0;append(msg,"Framebuffer base: ");hexnum(n,boot_info.framebuffer_base);append(msg,n);append(msg," bytes ");number(n,boot_info.framebuffer_size);append(msg,n);line_add(msg);return;}
    if(str_eq(token,"UPTIME")&&no_args){char msg[120]="TSC ticks since boot: ",n[24];number(n,rdtsc()-boot_tsc);append(msg,n);line_add(msg);msg[0]=0;append(msg,"PIT IRQ0 ticks (about 100 Hz): ");number(n,x86_timer_ticks());append(msg,n);line_add(msg);return;}
    if(str_eq(token,"VM")&&no_args){char msg[120]="Paging: ",n[24];number(n,vm_active_levels());append(msg,n);append(msg,"-level; NX ");append(msg,vm_nx_enabled()?"enabled":"unavailable");line_add(msg);return;}
    if(str_eq(token,"HEAP")&&no_args){kheap_stats_t s;kheap_get_stats(&s);char msg[120]="Heap VA: ",n[24];hexnum(n,s.base);append(msg,n);line_add(msg);msg[0]=0;append(msg,"Capacity ");number(n,s.capacity);append(msg,n);append(msg," bytes | used ");number(n,s.used_bytes);append(msg,n);append(msg," | free ");number(n,s.free_bytes);append(msg,n);line_add(msg);msg[0]=0;append(msg,"Live allocations: ");number(n,s.allocated_blocks);append(msg,n);line_add(msg);return;}
    if(str_eq(token,"THREADS")&&no_args){kthread_info_t list[KTHREAD_MAX];uint32_t count=thread_list(list,KTHREAD_MAX);line_add("ID   STATE       SWITCHES");for(uint32_t i=0;i<count&&i<KTHREAD_MAX;++i){char msg[120]="";char n[24];number(n,list[i].id);append(msg," ");append(msg,n);append(msg,"   ");append(msg,list[i].state==SCHED_RUNNING?"RUNNING":list[i].state==SCHED_READY?"READY":list[i].state==SCHED_BLOCKED?"BLOCKED":list[i].state==SCHED_TERMINATED?"TERMINATED":"UNUSED");append(msg,"   ");number(n,list[i].switches);append(msg,n);line_add(msg);}return;}
    if(str_eq(token,"SCHEDTEST")&&no_args){extern volatile uint64_t asterion_demo_progress[2];asterion_demo_progress[0]=asterion_demo_progress[1]=0;uint32_t a=thread_create(asterion_demo_worker,(void *)(uintptr_t)0),b=thread_create(asterion_demo_worker,(void *)(uintptr_t)1);char msg[120]="Created worker thread IDs ",n[24];if(!a||!b){line_add("SCHEDTEST failed: thread table or heap exhausted.");return;}number(n,a);append(msg,n);append(msg," and ");number(n,b);append(msg,n);line_add(msg);return;}
    if(str_eq(token,"ECHO")){line_add(args);return;}
    if(str_eq(token,"CALC")){calc_command(args);return;}
    if(str_eq(token,"COLOR")){char choice=upper(*args);const char *tail=skip_spaces(args+(*args?1:0));if(*tail||!console_set_theme(choice))line_add("Usage: COLOR G | A | B");else line_add(choice=='G'||choice=='g'?"OK: green accent":choice=='A'||choice=='a'?"OK: amber theme":"OK: blue theme");return;}
    if(str_eq(token,"LS")&&no_args){line_add("Built-in files:");line_add(" README.TXT   SYSTEM.TXT   LICENSE.TXT");line_add("Use CAT README to read the system guide.");return;}
    if(str_eq(token,"CAT")&&(str_eq_ci(args,"README")||str_eq_ci(args,"README.TXT"))){line_add("ASTERION OS: A freestanding x86_64 kernel prototype.");line_add("It takes over the UEFI framebuffer and polls PS/2 input.");line_add("Try HELP, MEM, CPU, PCI, CALC 6*7, COLOR G, and ECHO.");line_add("This build uses firmware for initial boot only.");return;}
    if(str_eq(token,"KEYS")&&no_args){line_add("Keyboard: PS/2 set-1 scan codes, polled at the i8042.");line_add("Enter submits, Backspace edits, Shift and Caps Lock work.");line_add("The VM should expose a PS/2 keyboard for this build.");return;}
    if(str_eq(token,"REBOOT")&&no_args){keyboard_reset_result_t result=keyboard_request_reset();if(result==KEYBOARD_RESET_SENT)line_add("i8042 reset command sent; firmware may reset the machine.");else if(result==KEYBOARD_RESET_UNAVAILABLE)line_add("REBOOT unavailable: no i8042 controller detected.");else line_add("REBOOT failed: i8042 input buffer did not become ready.");return;}
    if(str_eq(token,"HALT")&&no_args){line_add("CPU halted. Reset the virtual machine to continue.");console_render(input_line,shift_down);__asm__ volatile("cli");for(;;)__asm__ volatile("hlt");}
    char msg[120]="Unknown command: ";append(msg,p);append(msg,". Type HELP.");line_add(msg);
}
static char scan_to_ascii(uint8_t sc) {
    static const char normal[58]={0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '};
    static const char shifted[58]={0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S','D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '};
    if(sc>=58)return 0;
    char c=(shift_down?shifted:normal)[sc];
    if(c>='a'&&c<='z'&&caps_lock)c=(char)(c-'a'+'A');
    else if(c>='A'&&c<='Z'&&caps_lock&&!shift_down)c=(char)(c-'A'+'a');
    return c;
}
static void shell(void) {
    line_add("ASTERION // FREESTANDING KERNEL ONLINE");line_add("READY: firmware services released; direct console active.");
    line_add("Type HELP for commands. Your machine is not modified.");
    int controller_warning=0;
    console_render(input_line,shift_down);
    for(;;){
        thread_reap_terminated();
        uint8_t sc=0;keyboard_poll_result_t poll=keyboard_poll(&sc);
        if(poll==KEYBOARD_NO_DATA){__asm__ volatile("pause");continue;}
        if(poll==KEYBOARD_UNAVAILABLE){if(!controller_warning){line_add("WARN: i8042 keyboard controller unavailable.");controller_warning=1;console_render(input_line,shift_down);}__asm__ volatile("pause");continue;}
        if(poll==KEYBOARD_IO_ERROR){if(!controller_warning){line_add("WARN: i8042 reported a keyboard I/O error.");controller_warning=1;console_render(input_line,shift_down);}__asm__ volatile("pause");continue;}
        controller_warning=0;
        if(sc==0x2A||sc==0x36){shift_down=1;console_render(input_line,shift_down);continue;}if(sc==0xAA||sc==0xB6){shift_down=0;console_render(input_line,shift_down);continue;}if(sc==0x3A){caps_lock=!caps_lock;continue;}if(sc&0x80)continue;
        char c=scan_to_ascii(sc);if(c=='\n'){input_line[input_len]=0;line_add("ASTERION ::");line_add(input_line);run_command(input_line);input_len=0;input_line[0]=0;}
        else if(c=='\b'&&input_len){input_line[--input_len]=0;}
        else if(c=='\t'){static const char *h="HELP";if(input_len==0){copystr(input_line,h,sizeof(input_line));input_len=4;}}
        else if(c>=32&&c<127&&input_len<sizeof(input_line)-1){input_line[input_len++]=c;input_line[input_len]=0;}
        console_render(input_line,shift_down);
    }
}

void kernel_early_log(serial_level_t level, const char *message) {
    console_log(level, message);
}

void kernel_set_boot_tsc(uint64_t timestamp) { boot_tsc = timestamp; }

static void kernel_panic(const char *reason) {
    __asm__ volatile("cli");
    char context[64]="caller RIP ";hexnum(context+11,(uint64_t)(uintptr_t)__builtin_return_address(0));
    console_log(SERIAL_PANIC,"KERNEL PANIC");console_log(SERIAL_PANIC,reason);console_log(SERIAL_PANIC,context);console_render("",0);
    for(;;)__asm__ volatile("hlt");
}
void kernel_main(const boot_info_t *info) {
    __asm__ volatile("cli" ::: "memory");
    if (relocation_anchor != &boot_info) kernel_panic("PE image relocation check failed");
    if(!boot_info_validate(info))kernel_panic("invalid boot information, framebuffer, or memory-map contract");
    boot_info=*info;console_init(info);
    if (!x86_interrupts_init()) kernel_panic("CPU exception or timer interrupt self-test failed");
    if (!pmm_init(&boot_info)) kernel_panic("physical memory map is invalid or overlaps ambiguously");
    if (!pmm_self_test()) kernel_panic("physical frame allocator self-test failed");
    pmm_stats_t pmm_stats;pmm_get_stats(&pmm_stats);
    char pmm_msg[120]="PMM initialized: ",pmm_num[24];number(pmm_num,pmm_stats.free_pages);append(pmm_msg,pmm_num);append(pmm_msg," free frames (");number(pmm_num,pmm_stats.free_pages*PMM_PAGE_SIZE/(1024*1024));append(pmm_msg,pmm_num);append(pmm_msg," MiB); reserved regions excluded.");line_add(pmm_msg);
    line_add("SELFTEST PASS: physical frame alignment, free, bounded exhaustion, and invalid-operation checks.");
    if (!vm_init(&boot_info)) kernel_panic("could not safely replace inherited UEFI page tables");
    if (!vm_self_test()) kernel_panic("virtual memory map/unmap/page-fault self-test failed");
    if (!kheap_init()) kernel_panic("could not map fixed kernel heap region");
    if (!kheap_self_test()) kernel_panic("kernel heap self-test failed");
    line_add("SELFTEST PASS: heap alignment, reuse, coalescing, exhaustion, and invalid-free checks.");
    if (!threads_init()) kernel_panic("kernel thread scheduler initialization failed");
    if (!thread_scheduler_self_test()) kernel_panic("kernel thread preemption self-test failed");
    line_add("SELFTEST PASS: workers A and B completed; PIT preemption, voluntary yield, and round-robin verified.");
    char sched_msg[120]="Scheduler switches: ",sched_num[24];number(sched_num,thread_switch_count());append(sched_msg,sched_num);append(sched_msg," | PIT preemptions: ");number(sched_num,thread_preemption_count());append(sched_msg,sched_num);append(sched_msg," | voluntary: ");number(sched_num,thread_voluntary_switch_count());append(sched_msg,sched_num);line_add(sched_msg);
    block_device_t disk_device={0};
    if(storage_mount_aosfs(&disk_device,&rootfs)){rootfs_mounted=1;serial_log(SERIAL_INFO,"AHCI SATA block device mounted read-only AOSFS.");}else{serial_log(SERIAL_WARN,"AHCI/AOSFS data disk unavailable; disk ELF test will be skipped.");}
    if (!process_user_mode_self_test(rootfs_mounted ? &rootfs : 0)) kernel_panic("ring-3 process isolation self-test failed");
    line_add("CPU exceptions and PIT timer interrupt initialized; see serial self-test results.");
    line_add("ASTERION // FREESTANDING KERNEL ONLINE");
    line_add("READY: UEFI boot services exited; direct console active.");
    char msg[120]="BOOT INFO v1 | map descriptors: ",n[24];number(n,info->memory_descriptor_count);append(msg,n);line_add(msg);
    cpu_info();
    msg[0]=0;append(msg,"Conventional memory: ");number(n,info->conventional_memory_bytes/(1024*1024));append(msg,n);append(msg," MiB");line_add(msg);
    msg[0]=0;append(msg,"Framebuffer: ");number(n,info->framebuffer_width);append(msg,n);append(msg,"x");number(n,info->framebuffer_height);append(msg,n);append(msg," stride ");number(n,info->framebuffer_stride);append(msg,n);line_add(msg);
    line_add("Type HELP for commands. Kernel owns console output and input.");
    console_render(input_line,shift_down);shell();
}

