/* Real HLE/coherency integration: ordered producer data, not fabricated wakes. */
#include <assert.h>
#include <stdlib.h>
#include "../../../runtime/spu/spu_coherency.h"
#include "../cellSpursQueue.c"

uint8_t* vm_base;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea, uint64_t value, int width)
{ (void)ea; (void)value; (void)width; }
void spu_taskset_signal_task(uint32_t taskset, uint32_t task)
{ (void)taskset; (void)task; }
void spu_ch_wake(spu_context* ctx) { (void)ctx; }

enum { JOBS = 512, DEPTH = 8 };
static DWORD WINAPI producer(void* ignored)
{
    (void)ignored;
    for (u32 value = 0; value < JOBS; ++value) {
        for (;;) {
            spu_lockline_lock();
            u32 head = vm_read32(0x100), tail = vm_read32(0x104);
            u32 count = (tail + DEPTH * 2 - head) % (DEPTH * 2);
            if (count < DEPTH) {
                /* SPU-like publication: payload, index, then coherent wake. */
                u32 slot = 0x400 + (tail % DEPTH) * 16;
                vm_write32(slot, value);
                queue_write_index(vm_base + 0x104, (tail + 1) % (DEPTH * 2));
                spu_coh_notify_write_except(0x100, NULL);
                spu_lockline_unlock();
                break;
            }
            spu_lockline_unlock();
            SwitchToThread();
        }
        /* Alternate burst publication with a genuinely unavailable producer. */
        if ((value & 7) == 7) Sleep(1);
    }
    return 0;
}

int main(void)
{
    _putenv("SPURS_QUEUE_WAIT_NATIVE=1");
    vm_base = (uint8_t*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(vm_base);
    assert(_cellSpursQueueInitialize(0x800,0x900,0x100,0x400,16,DEPTH,1) == CELL_OK);
    HANDLE thread = CreateThread(NULL,0,producer,NULL,0,NULL);
    assert(thread);
    for (u32 expected = 0; expected < JOBS; ++expected) {
        assert(cellSpursQueuePopBody(0x100,0x300,0,1) == CELL_OK);
        assert(vm_read32(0x300) == expected);
    }
    assert(WaitForSingleObject(thread,1000)==WAIT_OBJECT_0);
    CloseHandle(thread);
    assert(vm_read32(0x100)==vm_read32(0x104));
    DWORD old; assert(VirtualProtect(vm_base,4096,PAGE_NOACCESS,&old));
    spu_lockline_lock(); spu_coh_notify_write(0x100); spu_lockline_unlock();
    VirtualFree(vm_base,0,MEM_RELEASE);
    puts("Native queue wake: 512 real results delivered in order; no retained queue mapping");
    return 0;
}
