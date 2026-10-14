/* Race task publication against sibling exits in the same bitset words. */
#include "../spu_taskset_dispatch.c"
#include "../../../libs/spurs/spurs_taskset.c"
#include <assert.h>
#include "../../platform/win32_compat.h"

static uint8_t memory[0x10000];
uint8_t* vm_base = memory;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
static SRWLOCK lock = SRWLOCK_INIT;
static _Thread_local int held;
static volatile LONG completed;
void spu_lockline_lock(void) { assert(!held); AcquireSRWLockExclusive(&lock); held = 1; }
void spu_lockline_unlock(void) { assert(held); held = 0; ReleaseSRWLockExclusive(&lock); }
void spu_coh_notify_write(uint32_t ea) { assert(held && ea >= 0x100 && ea < 0x1A00); }
int spu_coh_watch_taskset(uint32_t ea, uint32_t owner, void* host, void* event,
                          uint64_t (*qpc)(void))
{ assert(held); (void)ea; (void)owner; (void)host; (void)event; (void)qpc; return 0; }
void spu_coh_unwatch_tasksets(uint32_t owner) { assert(held); (void)owner; }
uint64_t spu_coh_taskset_ready_qpc_locked(uint32_t ea, uint32_t task)
{ assert(held); (void)ea; (void)task; return 0; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 1; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea, uint64_t value, int width)
{ (void)ea; (void)value; (void)width; }
size_t spu_elf_image_size(const uint8_t* p, size_t max)
{ assert(!held && p == memory + 0x4000 && max >= 16); return 16; }
uint64_t spu_workload_async_trace_mark(void) { return 0; }
int spu_workload_dispatch_async_task_provenance(const uint8_t* image, uint32_t size,
                                                uint32_t context, uint32_t ts,
                                                uint32_t task, uint32_t cause,
                                                uint64_t claim_qpc, uint64_t ready_qpc)
{
    assert(!held && image == memory + 0x4000 && size == 16);
    assert(context == 0x5000 && ts == 0x100 && task < 128);
    assert(cause <= SPU_TASKSET_DISPATCH_POLL && claim_qpc == 0 && ready_qpc == 0);
    return 1;
}
static DWORD WINAPI producer(LPVOID arg)
{
    const uint32_t owner = (uint32_t)(uintptr_t)arg;
    for (uint32_t n = 0; n < 5000; ++n) {
        const uint32_t args[4] = {owner, n, owner ^ n, 0x87654321};
        const uint32_t pattern[4] = {0x01020304, 0, 0xFEDCBA98, n};
        uint32_t task = spurs_taskset_create_task(0x100, 0x4000, 0x507A, args, pattern);
        assert(task < 128);
        if ((n & 7) == 0) SwitchToThread();
        spu_lockline_lock();
        uint32_t ti = spurs_taskset_taskinfo_ea(0x100, task);
        assert(spurs_bitset_test(0x100 + CSTS_ENABLED, task));
        for (unsigned i = 0; i < 4; ++i) {
            assert(vm_read32(ti + TI_ARGS + 4*i) == args[i]);
            assert(vm_read32(ti + TI_LS_PATTERN + 4*i) == pattern[i]);
        }
        assert(vm_read64(ti + TI_ELF) == 0x4000);
        assert(vm_read64(ti + TI_CONTEXT) == 0x507A);
        spu_lockline_unlock();
        spu_taskset_task_exited(0x100, task);
        InterlockedIncrement(&completed);
    }
    return 0;
}
int main(void)
{
    /* Capacity and the full result are independent of host task timing. */
    for (uint32_t i = 0; i < 128; ++i)
        assert(spurs_taskset_create_task(0x100, 0x4000, 0x507A, NULL, NULL) == i);
    assert(spurs_taskset_create_task(0x100, 0x4000, 0x507A, NULL, NULL) == 128);
    for (uint32_t i = 0; i < 128; ++i) spu_taskset_task_exited(0x100, i);
    HANDLE threads[8];
    for (unsigned i = 0; i < 8; ++i) {
        threads[i] = CreateThread(NULL, 0, producer, (LPVOID)(uintptr_t)(i + 1), 0, NULL);
        assert(threads[i]);
    }
    assert(WaitForMultipleObjects(8, threads, TRUE, 30000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 8; ++i) CloseHandle(threads[i]);
    assert(completed == 40000);
    for (uint32_t i = 0; i < 128; ++i) {
        assert(!spurs_bitset_test(0x100 + CSTS_ENABLED, i));
        assert(!spurs_bitset_test(0x100 + CSTS_READY, i));
    }
    puts("Taskset atomic publication: 40,000 concurrent create/exit cycles passed");
    return 0;
}
