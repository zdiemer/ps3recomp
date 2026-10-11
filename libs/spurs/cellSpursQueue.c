/* Read-only SPURS queue query over the BE ring shared with lifted SPU code. */
#include "cellSpurs.h"
#include "../../runtime/ppu/ppu_memory.h"

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
