/* SPURS queue operations over the BE ring shared with lifted SPU code. */
#include "cellSpurs.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../runtime/platform/win32_compat.h"
#include "spurs_queue_profile.h"
#include "spurs_queue_wait.h"

/* Shared with the SDK's SPU queue routines: W0 pop, W1 push, W2 entry size,
 * W3 depth; the next quadword starts with the 64-bit buffer EA and ends with
 * direction. The old initializer put size at +0x10 and buffer at +0x18:
 * Haze's consumer consequently issued size=0, EA=0x0000001000000080. */
s32 _cellSpursQueueInitialize(u64 spurs_ea, u64 taskset_ea, u64 queue_ea,
                            u64 buffer_ea, u32 size, u32 depth, u32 direction)
{
    (void)spurs_ea;
    u32 q = (u32)queue_ea, buffer = (u32)buffer_ea;
    if (!q || !buffer) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if ((q & 127u) || (buffer & 15u)) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (!size || (size & 15u) || !depth || depth > 0x3FFFFFFFu || direction > 2 ||
        (u64)buffer + (u64)size * depth > 0x100000000ull)
        return CELL_SPURS_TASK_ERROR_INVAL;

    memset(vm_base + q, 0, 128);
    vm_write32(q + 0x08, size);
    vm_write32(q + 0x0C, depth);
    vm_write64(q + 0x10, buffer_ea);
    vm_write32(q + 0x1C, direction);
    vm_write64(q + 0x70, taskset_ea);
    return CELL_OK;
}

void spu_lockline_lock(void);
void spu_lockline_unlock(void);
void spu_coh_notify_write(uint32_t ea);
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);

static void queue_write_index(uint8_t* p, u32 index)
{
    p[0] = (uint8_t)(index >> 24); p[1] = (uint8_t)(index >> 16);
    p[2] = (uint8_t)(index >> 8); p[3] = (uint8_t)index;
}

/* SDK SPU routines use count + 15 byte IDs, followed by a parallel workload-ID
 * ring. Remove just one waiter after publication and signal outside the lock. */
static int queue_take_waiter(uint8_t* q, unsigned offset)
{
    unsigned count = q[offset];
    if (!count || count > 15) return -1;
    int task = q[offset + 1];
    memmove(q + offset + 1, q + offset + 2, 14);
    q[offset + 15] = 0;
    q[offset] = (uint8_t)(count - 1);
    memmove(q + offset + 0x21, q + offset + 0x22, 14);
    q[offset + 0x2F] = 0;
    q[offset + 0x20] = (uint8_t)(count - 1);
    return task;
}

static s32 queue_transfer(u64 queue_ea, u64 data_ea, u32 blocking, int push, u32 peek)
{
    u32 q = (u32)queue_ea, data = (u32)data_ea;
    if (!q || !data) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if ((q & 127u) || (data & 15u)) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (blocking > 1 || peek > 1 || queue_ea > UINT32_MAX || data_ea > UINT32_MAX)
        return CELL_SPURS_TASK_ERROR_INVAL;

    QUEUE_PROFILE_BEGIN();
    int native_wait = blocking && queue_native_wait_enabled();
    int queue_watch = -1;
    for (;;) {
        QUEUE_PROFILE_LOCK_BEGIN();
        spu_lockline_lock();
        QUEUE_PROFILE_LOCK_END();
        u32 head = vm_read32(q), tail = vm_read32(q + 4);
        u32 size = vm_read32(q + 8), depth = vm_read32(q + 12);
        u64 buffer64 = vm_read64(q + 16);
        u32 direction = vm_read32(q + 28);
        if (!size || (size & 15u) || !depth || depth > 0x3FFFFFFFu ||
            !buffer64 || buffer64 > UINT32_MAX || (buffer64 & 15u) ||
            buffer64 + (u64)size * depth > 0x100000000ull ||
            (u64)data + size > 0x100000000ull || direction > 2) {
            if (queue_watch >= 0) spu_coh_unwatch_queue(queue_watch);
            spu_lockline_unlock();
            QUEUE_RETURN(CELL_SPURS_TASK_ERROR_INVAL);
        }
        if (direction != (push ? 2u : 1u)) {
            if (queue_watch >= 0) spu_coh_unwatch_queue(queue_watch);
            spu_lockline_unlock();
            QUEUE_RETURN(CELL_SPURS_TASK_ERROR_PERM);
        }
        u32 normalized_head = (head & 0x80000000u) ? ~head : head;
        u32 normalized_tail = (tail & 0x80000000u) ? ~tail : tail;
        u32 modulus = depth * 2;
        if (normalized_head >= modulus || normalized_tail >= modulus) {
            if (queue_watch >= 0) spu_coh_unwatch_queue(queue_watch);
            spu_lockline_unlock();
            QUEUE_RETURN(CELL_SPURS_TASK_ERROR_INVAL);
        }
        u32 count = normalized_tail >= normalized_head ? normalized_tail - normalized_head
                  : modulus - normalized_head + normalized_tail;
        if (count > depth) {
            if (queue_watch >= 0) spu_coh_unwatch_queue(queue_watch);
            spu_lockline_unlock();
            QUEUE_RETURN(CELL_SPURS_TASK_ERROR_INVAL);
        }
        int busy = (head | tail) & 0x80000000u;
        if (busy || (push ? count == depth : count == 0)) {
            QUEUE_PROFILE_TASKSET((u32)vm_read64(q + 0x70));
            QUEUE_PROFILE_WAIT(busy);
            u64 snapshot = 0;
            if (native_wait) {
                memcpy(&snapshot, vm_base + q, sizeof(snapshot));
                if (queue_watch < 0) queue_watch = spu_coh_watch_queue(q, vm_base + q);
            }
            spu_lockline_unlock();
            if (!blocking) QUEUE_RETURN(busy ? CELL_SPURS_TASK_ERROR_BUSY : CELL_SPURS_TASK_ERROR_AGAIN);
            QUEUE_PROFILE_SLEEP(queue_wait_for_publication(q, snapshot, native_wait));
            continue;
        }
        u32 index = push ? tail : head;
        uint8_t* slot = vm_base + (u32)buffer64 + (size_t)(index % depth) * size;
        if (push) memcpy(slot, vm_base + data, size);
        else memcpy(vm_base + data, slot, size);
        if (peek) {
            if (queue_watch >= 0) spu_coh_unwatch_queue(queue_watch);
            spu_lockline_unlock();
            QUEUE_RETURN(CELL_OK);
        }
        /* Payload precedes publication under the same lock as GETLLAR/PUTLLC. */
        queue_write_index(vm_base + q + (push ? 4 : 0), (index + 1) % modulus);
        u32 taskset = (u32)vm_read64(q + 0x70);
        QUEUE_PROFILE_TASKSET(taskset);
        int waiter = queue_take_waiter(vm_base + q, push ? 0x20 : 0x30);
        spu_coh_notify_write(q);
        if (queue_watch >= 0) spu_coh_unwatch_queue(queue_watch);
        spu_lockline_unlock();
        if (waiter >= 0 && taskset) spu_taskset_signal_task(taskset, (u32)waiter);
        QUEUE_RETURN(CELL_OK);
    }
}

s32 cellSpursQueuePushBody(u64 queue_ea, u64 data_ea, u32 isBlocking)
{
    return queue_transfer(queue_ea, data_ea, isBlocking, 1, 0);
}

s32 cellSpursQueuePopBody(u64 queue_ea, u64 data_ea, u32 isPeek, u32 isBlocking)
{
    /* Retail r5 selects Peek; blocking is the fourth argument in r6. */
    return queue_transfer(queue_ea, data_ea, isBlocking, 0, isPeek);
}

/* The queue initializer and the SPU consumer use W0/W1 as pop/push indices,
 * with negative values encoding an in-progress claim as one's complement.
 * W3 is the depth; indices wrap at twice the depth to distinguish full/empty.
 * Take the SPU atomic-line lock for a consistent snapshot, then release it
 * before vm_write32 (which may itself take that lock for a reserved output).
 * This query does not enqueue, dequeue, or change waiter state. */
s32 cellSpursQueueSize(u64 queue_ea, u64 size_out_ea)
{
    u32 q = (u32)queue_ea, out = (u32)size_out_ea;
    if (!q || !out) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if ((q & 127u) || (out & 3u)) return CELL_SPURS_TASK_ERROR_ALIGN;

    spu_lockline_lock();
    u32 head = vm_read32(q), tail = vm_read32(q + 4), depth = vm_read32(q + 12);
    spu_lockline_unlock();

    if (!depth || depth > 0x3FFFFFFFu) return CELL_SPURS_TASK_ERROR_INVAL;
    if (head & 0x80000000u) head = ~head;
    if (tail & 0x80000000u) tail = ~tail;
    u32 modulus = depth * 2u;
    if (head >= modulus || tail >= modulus) return CELL_SPURS_TASK_ERROR_INVAL;
    u32 count = tail >= head ? tail - head : modulus - head + tail;
    if (count > depth) return CELL_SPURS_TASK_ERROR_INVAL;
    vm_write32(out, count);
    return CELL_OK;
}
