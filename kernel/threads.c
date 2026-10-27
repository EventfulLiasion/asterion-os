#include <stddef.h>
#include <stdint.h>
#include "threads.h"
#include "heap.h"
#include "../arch/x86_64/interrupts.h"
#include "../drivers/serial.h"

typedef struct { void *saved_frame,*stack; kthread_entry_t entry; void *argument; uint64_t switches; } kthread_t;

static sched_slot_t slots[KTHREAD_MAX];
static kthread_t threads[KTHREAD_MAX];
static sched_queue_t ready;
static uint32_t current_slot,next_id=1,timer_preemptions,voluntary_switches;
static uint64_t total_switches;
static int initialized;
extern void x86_thread_bootstrap(void);

static void log_hex(serial_level_t level, const char *label, uint64_t value) {
    static const char digits[]="0123456789ABCDEF"; char message[64]; size_t n=0;
    while(label[n]&&n<sizeof(message)-19){message[n]=label[n];++n;}
    message[n++]='0';message[n++]='x';
    for(int shift=60;shift>=0;shift-=4)message[n++]=digits[(value>>shift)&15];
    message[n]=0;serial_log(level,message);
}

static uint64_t irq_save(void) { uint64_t f; __asm__ volatile("pushfq; popq %0; cli":"=r"(f)::"memory"); return f; }
static void irq_restore(uint64_t f) { if(f&(UINT64_C(1)<<9)) __asm__ volatile("sti":::"memory"); }

int threads_init(void) {
    if(initialized||!sched_queue_init(&ready,slots,KTHREAD_MAX)) return 0;
    slots[0].id=0; slots[0].state=SCHED_RUNNING; slots[0].next_ready=-1;
    current_slot=0; initialized=1;
    serial_log(SERIAL_INFO,"Thread scheduler initialized; boot shell is the idle thread.");
    return 1;
}

uint32_t thread_create(kthread_entry_t entry,void *argument) {
    if(!initialized||!entry) return 0;
    uint64_t f=irq_save(); uint32_t i;
    for(i=1;i<KTHREAD_MAX&&slots[i].state!=SCHED_UNUSED;++i) {}
    if(i==KTHREAD_MAX) { irq_restore(f); return 0; }
    void *stack=kmalloc(KTHREAD_STACK_SIZE);
    if(!stack) { irq_restore(f); return 0; }
    uintptr_t top=((uintptr_t)stack+KTHREAD_STACK_SIZE)&~(uintptr_t)15;
    x86_interrupt_return_frame_t *frame=(x86_interrupt_return_frame_t *)(top-sizeof(*frame));
    x86_interrupt_return_frame_init(frame,(uintptr_t)x86_thread_bootstrap,0x08,0x202,top,0x10);
    threads[i].saved_frame=frame; threads[i].stack=stack; threads[i].entry=entry;
    threads[i].argument=argument; threads[i].switches=0;
    uint32_t id=next_id++;
    if (!id) id=next_id++;
    for (uint32_t attempts=0;attempts<KTHREAD_MAX;++attempts) {
        int collision=0;
        for (uint32_t j=0;j<KTHREAD_MAX;++j)
            if (slots[j].state!=SCHED_UNUSED && slots[j].id==id) { collision=1; break; }
        if (!collision) break;
        id=next_id++; if (!id) id=next_id++;
    }
    for (uint32_t j=0;j<KTHREAD_MAX;++j) if (slots[j].state!=SCHED_UNUSED && slots[j].id==id) {
        slots[i].state=SCHED_UNUSED; threads[i].stack=NULL; (void)kfree(stack); irq_restore(f); return 0;
    }
    slots[i].id=id; slots[i].state=SCHED_READY; slots[i].next_ready=-1;
    if(!sched_queue_push(&ready,i)) { slots[i].state=SCHED_UNUSED; threads[i].stack=NULL; (void)kfree(stack); irq_restore(f); return 0; }
    id=slots[i].id; irq_restore(f); return id;
}

void *thread_schedule_interrupt(void *frame,uint64_t vector) {
    if(!initialized) return frame;
    x86_interrupt_return_frame_t *incoming=(x86_interrupt_return_frame_t *)frame;
    if (!x86_interrupt_return_frame_valid(incoming,0x08,0x10)) {
        serial_log(SERIAL_PANIC,"Scheduler received malformed same-ring interrupt frame.");
        log_hex(SERIAL_PANIC,"interrupt vector=",vector);
        log_hex(SERIAL_PANIC,"interrupt frame=",(uintptr_t)incoming);
        if (incoming) { log_hex(SERIAL_PANIC,"saved RIP=",incoming->rip); log_hex(SERIAL_PANIC,"saved CS=",incoming->cs); }
        __asm__ volatile("cli"); for(;;)__asm__ volatile("hlt");
    }
    uint32_t old=current_slot;
    threads[old].saved_frame=frame;
    if(slots[old].state==SCHED_RUNNING) {
        slots[old].state=SCHED_READY;
        if(!sched_queue_push(&ready,old)) { serial_log(SERIAL_PANIC,"Scheduler ready queue invariant failed."); __asm__ volatile("cli"); for(;;)__asm__ volatile("hlt"); }
    }
    uint32_t next;
    if(!sched_queue_pop(&ready,&next)) {
        if(slots[old].state==SCHED_READY) { (void)sched_transition(&ready,old,SCHED_READY,SCHED_RUNNING); current_slot=old; return frame; }
        serial_log(SERIAL_PANIC,"No runnable kernel thread; idle thread unavailable."); __asm__ volatile("cli"); for(;;)__asm__ volatile("hlt");
    }
    if(!sched_transition(&ready,next,SCHED_READY,SCHED_RUNNING)) { serial_log(SERIAL_PANIC,"Scheduler selected a non-ready thread."); __asm__ volatile("cli"); for(;;)__asm__ volatile("hlt"); }
    current_slot=next; ++threads[next].switches; ++total_switches;
    if(vector==32&&next!=old) ++timer_preemptions;
    if(vector==48&&next!=old) ++voluntary_switches;
    x86_interrupt_return_frame_t *return_frame=(x86_interrupt_return_frame_t *)threads[next].saved_frame;
    if (!x86_interrupt_return_frame_valid(return_frame,0x08,0x10)) {
        serial_log(SERIAL_PANIC,"Scheduler rejected malformed same-ring return frame before IRETQ.");
        log_hex(SERIAL_PANIC,"target thread ID=",slots[next].id);
        log_hex(SERIAL_PANIC,"return frame=",(uintptr_t)return_frame);
        if (return_frame) { log_hex(SERIAL_PANIC,"return RIP=",return_frame->rip); log_hex(SERIAL_PANIC,"return CS=",return_frame->cs); }
        __asm__ volatile("cli"); for(;;)__asm__ volatile("hlt");
    }
    return return_frame;
}

void thread_start_current(void) {
    kthread_entry_t entry=threads[current_slot].entry; void *arg=threads[current_slot].argument;
    if(entry) entry(arg);
    thread_exit();
}
void thread_yield(void) { if(initialized) __asm__ volatile("int $0x30":::"memory"); }
void thread_exit(void) {
    uint64_t f=irq_save();
    if(!initialized||current_slot==0||slots[current_slot].state!=SCHED_RUNNING) { irq_restore(f); serial_log(SERIAL_PANIC,"Invalid kernel thread termination."); __asm__ volatile("cli"); for(;;)__asm__ volatile("hlt"); }
    slots[current_slot].state=SCHED_TERMINATED; (void)f;
    __asm__ volatile("int $0x30":::"memory");
    serial_log(SERIAL_PANIC,"Terminated thread resumed unexpectedly."); __asm__ volatile("cli"); for(;;)__asm__ volatile("hlt");
}

void thread_reap_terminated(void) {
    uint64_t f=irq_save();
    for(uint32_t i=1;i<KTHREAD_MAX;++i) if(slots[i].state==SCHED_TERMINATED) {
        void *stack=threads[i].stack; slots[i].id=0; slots[i].state=SCHED_UNUSED; slots[i].next_ready=-1;
        threads[i].stack=NULL; threads[i].saved_frame=NULL; threads[i].entry=NULL; threads[i].argument=NULL;
        if(stack&&!kfree(stack)) serial_log(SERIAL_ERROR,"Could not reclaim terminated thread stack.");
    }
    irq_restore(f);
}
uint32_t thread_list(kthread_info_t *out,uint32_t cap) {
    uint64_t f=irq_save(); uint32_t n=0;
    for(uint32_t i=0;i<KTHREAD_MAX;++i) if(slots[i].state!=SCHED_UNUSED) { if(out&&n<cap) out[n]=(kthread_info_t){slots[i].id,slots[i].state,threads[i].switches}; ++n; }
    irq_restore(f); return n;
}
uint32_t thread_preemption_count(void) { return timer_preemptions; }
uint32_t thread_voluntary_switch_count(void) { return voluntary_switches; }
uint64_t thread_switch_count(void) { return total_switches; }

static volatile uint64_t progress[2];
static void test_worker(void *arg) { uint32_t n=(uint32_t)(uintptr_t)arg; for(uint64_t i=0;i<UINT64_C(30000000);++i) { progress[n]=i+1; if((i%UINT64_C(250000))==0) thread_yield(); } }
int thread_scheduler_self_test(void) {
    progress[0]=progress[1]=0; uint32_t before=thread_preemption_count(),yield_before=thread_voluntary_switch_count();
    if(!thread_create(test_worker,(void *)(uintptr_t)0)||!thread_create(test_worker,(void *)(uintptr_t)1)) return 0;
    uint64_t start=x86_timer_ticks();
    while((progress[0]<UINT64_C(30000000)||progress[1]<UINT64_C(30000000))&&x86_timer_ticks()-start<1000) __asm__ volatile("pause");
    thread_reap_terminated();
    return progress[0]==UINT64_C(30000000)&&progress[1]==UINT64_C(30000000)&&
           thread_preemption_count()>before&&thread_voluntary_switch_count()>yield_before;
}
