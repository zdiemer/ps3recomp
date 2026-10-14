/* Synthetic scheduler contract: persistent parents do not starve ready tasks. */
#include "../spu_taskset_dispatch.c"
#include <assert.h>

static uint8_t memory[0x10000];
uint8_t* vm_base = memory;
static int locked, notifications, dispatches, fail_dispatch;
static uint32_t last_task, last_ts, last_context;
static uint32_t last_cause;
static uint64_t last_claim_qpc;
static uint64_t mark_counter;
void spu_lockline_lock(void) { assert(!locked); locked = 1; }
void spu_lockline_unlock(void) { assert(locked); locked = 0; }
void spu_coh_notify_write(uint32_t ea) { assert(locked && ea == 0x100); ++notifications; }
int spu_coh_watch_taskset(uint32_t ea, uint32_t owner, void* host, void* event,
                          uint64_t (*qpc)(void))
{ assert(locked); (void)ea; (void)owner; (void)host; (void)event; (void)qpc; return 0; }
void spu_coh_unwatch_tasksets(uint32_t owner) { assert(locked); (void)owner; }
uint64_t spu_coh_taskset_ready_qpc_locked(uint32_t ea, uint32_t task)
{ assert(locked); (void)ea; (void)task; return 0; }
size_t spu_elf_image_size(const uint8_t* p, size_t max)
{ assert(!locked && p == memory + 0x4000 && max > 16); return 16; }
uint64_t spu_workload_async_trace_mark(void) { return ++mark_counter + 1000; }
int spu_workload_dispatch_async_task_provenance(const uint8_t* p, uint32_t size,
                                                uint32_t context, uint32_t ts,
                                                uint32_t task, uint32_t cause,
                                                uint64_t claim_qpc, uint64_t ready_qpc)
{
    assert(!locked && p == memory + 0x4000 && size == 16);
    ++dispatches; last_task = task; last_ts = ts; last_context = context;
    last_cause = cause; last_claim_qpc = claim_qpc; (void)ready_qpc;
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
    assert(last_cause == SPU_TASKSET_DISPATCH_POLL && last_claim_qpc == 1001);
    spu_taskset_poll(0x2000);
    assert(dispatches == 1); /* claimed before asynchronous start */
    spu_taskset_task_exited(0x100, 1);
    assert(notifications == 1 && !(memory[0x100 + CSTS_ENABLED] & 0x40));
    ready(5);
    spu_taskset_task_exited(0x100, 1);
    assert(dispatches == 2 && last_task == 5 &&
           last_cause == SPU_TASKSET_DISPATCH_TASK_EXIT && last_claim_qpc == 1002);
    /* A task can finish between publication and CreateTask's scheduling call.
     * Scheduling again must not launch a saved, already completed ELF. */
    spu_taskset_start_ready(0x100);
    assert(dispatches == 2);
    ready(1);
    spu_taskset_poll(0x2000);
    assert(dispatches == 3 && last_task == 1); /* task IDs recycle */
    assert(last_cause == SPU_TASKSET_DISPATCH_POLL && last_claim_qpc == 1003);
    ready(2);
    fail_dispatch = 1;
    spu_taskset_poll(0x2000);
    assert(dispatches == 4);
    fail_dispatch = 0;
    spu_taskset_poll(0x2000);
    assert(dispatches == 5 && last_task == 2); /* failed launch retried */
    assert(last_cause == SPU_TASKSET_DISPATCH_POLL && last_claim_qpc == 1005);
    spu_taskset_poll(0x2000);
    assert(dispatches == 5);
    ready(6);
    spu_taskset_start_ready_cause(0x100, SPU_TASKSET_DISPATCH_CREATE);
    assert(dispatches == 6 && last_task == 6 &&
           last_cause == SPU_TASKSET_DISPATCH_CREATE && last_claim_qpc == 1006);
    ready(7);
    spu_taskset_poll_cause(0x2000, SPU_TASKSET_DISPATCH_WAKE);
    assert(dispatches == 7 && last_task == 7 &&
           last_cause == SPU_TASKSET_DISPATCH_WAKE && last_claim_qpc == 1007);
    uint32_t ti = 0x100 + CSTS_TASKINFO + TI_SIZE;
    const uint32_t expected[4] = {0x12000, 0, 0x34000, 0x56000};
    for (unsigned i = 0; i < 4; ++i) write_be32(ti + TI_ARGS + 4*i, expected[i]);
    uint32_t captured[4];
    spu_taskset_capture_args(0x100, 1, captured);
    memset(memory + ti + TI_ARGS, 0xCD, 16); /* next task overwrites its slot */
    for (unsigned i = 0; i < 4; ++i) assert(captured[i] == expected[i]);
    puts("Taskset dispatch contract passed");
    return 0;
}
