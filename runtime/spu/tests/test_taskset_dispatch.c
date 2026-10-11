/* Synthetic scheduler contract: persistent parents do not starve ready tasks. */
#include "../spu_taskset_dispatch.c"
#include <assert.h>

static uint8_t memory[0x10000];
uint8_t* vm_base = memory;
static int locked, notifications, dispatches, fail_dispatch;
static uint32_t last_task, last_ts, last_context;
void spu_lockline_lock(void) { assert(!locked); locked = 1; }
void spu_lockline_unlock(void) { assert(locked); locked = 0; }
void spu_coh_notify_write(uint32_t ea) { assert(locked && ea == 0x100); ++notifications; }
size_t spu_elf_image_size(const uint8_t* p, size_t max)
{ assert(!locked && p == memory + 0x4000 && max > 16); return 16; }
int spu_workload_dispatch_async_task(const uint8_t* p, uint32_t size, uint32_t context,
                                    uint32_t ts, uint32_t task)
{
    assert(!locked && p == memory + 0x4000 && size == 16);
    ++dispatches; last_task = task; last_ts = ts; last_context = context;
    return !fail_dispatch;
}
static void write_be32(uint32_t ea, uint32_t v)
{
    memory[ea] = (uint8_t)(v >> 24); memory[ea+1] = (uint8_t)(v >> 16);
    memory[ea+2] = (uint8_t)(v >> 8); memory[ea+3] = (uint8_t)v;
}
static void ready(uint32_t task)
{
    uint8_t mask = (uint8_t)(0x80u >> (task & 7));
    memory[0x100 + CSTS_ENABLED + task / 8] |= mask;
    memory[0x100 + CSTS_PENDING_READY + task / 8] |= mask;
    uint32_t ti = 0x100 + CSTS_TASKINFO + task * TI_SIZE;
    write_be32(ti + TI_ELF + 4, 0x4003); /* ELF flags must be stripped */
    write_be32(ti + TI_CONTEXT + 4, 0x507A); /* save-block count in low bits */
}
int main(void)
{
    write_be32(0x100 + CSTS_SPURS + 4, 0x2000);
    ready(0);
    assert(spu_taskset_claim(0x100, 0)); /* persistent parent already running */
    assert(!spu_taskset_claim(0x100, 0));
    ready(1);
    spu_taskset_poll(0x3000); /* other SPURS instance */
    assert(dispatches == 0);
    spu_taskset_poll(0x2000);
    assert(dispatches == 1 && last_ts == 0x100 && last_task == 1 && last_context == 0x5000);
    spu_taskset_poll(0x2000);
    assert(dispatches == 1); /* claimed before asynchronous start */
    spu_taskset_task_exited(0x100, 1);
    assert(notifications == 1 && !(memory[0x100 + CSTS_ENABLED] & 0x40));
    ready(1);
    spu_taskset_poll(0x2000);
    assert(dispatches == 2 && last_task == 1); /* task IDs recycle */
    ready(2);
    fail_dispatch = 1;
    spu_taskset_poll(0x2000);
    assert(dispatches == 3);
    fail_dispatch = 0;
    spu_taskset_poll(0x2000);
    assert(dispatches == 4 && last_task == 2); /* failed launch retried */
    spu_taskset_poll(0x2000);
    assert(dispatches == 4);
    puts("Taskset dispatch contract passed");
    return 0;
}
