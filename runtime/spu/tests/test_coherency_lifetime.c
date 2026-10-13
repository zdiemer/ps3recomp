/* Public-API regression: holes must not duplicate a later holder, and all live
 * holders must receive reservation loss even when there are more than eight. */
#include "../spu_coherency.h"
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned wakes;
static void notify(uint32_t ea);
#pragma comment(lib, "synchronization.lib")
static HANDLE waiter_started;
static DWORD WINAPI queue_waiter(void* address) {
    uint64_t snapshot = 0;
    SetEvent(waiter_started);
    return WaitOnAddress(address, &snapshot, 8, 2000) ? 0 : GetLastError();
}
static void test_queue_wake(int except) {
    uint64_t* header = (uint64_t*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(header);
    spu_lockline_lock();
    int first = spu_coh_watch_queue(0x4000, header);
    int second = spu_coh_watch_queue(0x4000, header);
    assert(first >= 0 && second == first);
    spu_coh_unwatch_queue(first); /* another caller still waits */
    spu_lockline_unlock();
    waiter_started = CreateEvent(NULL,TRUE,FALSE,NULL);
    HANDLE waiter = CreateThread(NULL,0,queue_waiter,header,0,NULL);
    assert(waiter && waiter_started);
    assert(WaitForSingleObject(waiter_started,1000)==WAIT_OBJECT_0);
    Sleep(20);
    spu_lockline_lock();
    *header = 1;
    if (except) spu_coh_notify_write_except(0x4000,NULL);
    else spu_coh_notify_write(0x4000);
    spu_lockline_unlock();
    assert(WaitForSingleObject(waiter,1000)==WAIT_OBJECT_0);
    DWORD result; assert(GetExitCodeThread(waiter,&result) && result==0);
    CloseHandle(waiter); CloseHandle(waiter_started);
    spu_lockline_lock(); spu_coh_unwatch_queue(second); spu_lockline_unlock();
    DWORD old; assert(VirtualProtect(header,4096,PAGE_NOACCESS,&old));
    notify(0x4000); /* removed watches must not dereference a freed queue */
    VirtualFree(header,0,MEM_RELEASE);
}
void spu_ch_wake(spu_context* ctx) { assert(ctx); ++wakes; }
static void reserve(spu_context* ctx,uint32_t ea) {
    spu_lockline_lock(); ctx->resv_valid=1; ctx->resv_ea=ea;
    spu_coh_reserve(ctx,ea); spu_lockline_unlock();
}
static void notify(uint32_t ea) {
    spu_lockline_lock(); spu_coh_notify_write(ea); spu_lockline_unlock();
}
int main(void) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    spu_context* first=(spu_context*)VirtualAlloc(NULL,sizeof(*first),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    spu_context* later=(spu_context*)VirtualAlloc(NULL,sizeof(*later),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(first && later);
    reserve(first,0x4000); reserve(later,0x4000);
    spu_coh_unregister(first); /* hole before later's slot in the old registry */
    reserve(later,0x4000); reserve(later,0x4000);
    spu_coh_unregister(later);
    DWORD old; assert(VirtualProtect(later,sizeof(*later),PAGE_NOACCESS,&old));
    notify(0x4000); /* old code reads the duplicate dangling holder here */
    assert(wakes==0);
    VirtualFree(first,0,MEM_RELEASE); VirtualFree(later,0,MEM_RELEASE);

    enum { COUNT=128 };
    spu_context* holders=(spu_context*)_aligned_malloc(COUNT*sizeof(*holders),16); assert(holders);
    memset(holders,0,COUNT*sizeof(*holders));
    for (unsigned i=0;i<COUNT;i++) reserve(&holders[i],0x5000);
    spu_lockline_lock(); spu_coh_notify_write_except(0x5000,&holders[17]); spu_lockline_unlock();
    assert(wakes==COUNT-1);
    for (unsigned i=0;i<COUNT;i++) {
        assert(holders[i].resv_valid==(i==17));
        assert((holders[i].event_status & SPU_EVENT_LR)==(i==17 ? 0 : SPU_EVENT_LR));
    }
    notify(0x5000); assert(wakes==COUNT);
    for (unsigned i=0;i<COUNT;i++) spu_coh_unregister(&holders[i]);
    _aligned_free(holders); notify(0x5000); assert(wakes==COUNT);
    test_queue_wake(0); test_queue_wake(1);
    puts("SPU coherency: reservation lifetimes and native queue wake/refcount lifetimes passed");
    return 0;
}
