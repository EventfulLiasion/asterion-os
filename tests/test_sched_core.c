#include <assert.h>
#include "../kernel/sched_core.h"

int main(void) {
    sched_slot_t slots[4]; sched_queue_t q; uint32_t out=99;
    assert(!sched_queue_init(NULL,slots,4));
    assert(!sched_queue_init(&q,NULL,4));
    assert(sched_queue_init(&q,slots,4));
    assert(!sched_queue_pop(&q,&out));
    slots[1].id=1; slots[1].state=SCHED_READY; slots[1].next_ready=-1;
    slots[2].id=2; slots[2].state=SCHED_READY; slots[2].next_ready=-1;
    assert(sched_queue_push(&q,1)); assert(!sched_queue_push(&q,1));
    assert(sched_queue_push(&q,2)); assert(q.ready_count==2);
    assert(sched_queue_pop(&q,&out)&&out==1);
    assert(sched_transition(&q,1,SCHED_READY,SCHED_RUNNING));
    assert(!sched_transition(&q,1,SCHED_READY,SCHED_TERMINATED));
    assert(sched_queue_pop(&q,&out)&&out==2);
    assert(sched_transition(&q,2,SCHED_READY,SCHED_TERMINATED));
    assert(!sched_queue_push(&q,2));
    assert(q.ready_count==0&&q.ready_head==-1&&q.ready_tail==-1);
    assert(!sched_transition(&q,4,SCHED_READY,SCHED_RUNNING));
    return 0;
}
