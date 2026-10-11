#ifndef PS3RECOMP_WIN32_USLEEP_H
#define PS3RECOMP_WIN32_USLEEP_H
#ifdef _WIN32
#include <windows.h>
#include <stdint.h>

/* Opt-in experiment: park short guest waits instead of occupying a host core.
 * FLS closes each reusable timer when its host thread exits. Unsupported OS
 * versions or failed timer calls fall back to the existing implementation. */
static INIT_ONCE ps3_usleep_once = INIT_ONCE_STATIC_INIT;
static DWORD ps3_usleep_slot = FLS_OUT_OF_INDEXES;
static VOID CALLBACK ps3_usleep_destroy(PVOID timer) { if (timer) CloseHandle(timer); }
static BOOL CALLBACK ps3_usleep_initialize(PINIT_ONCE once, PVOID param, PVOID* context)
{
    (void)once; (void)param; (void)context;
    ps3_usleep_slot = FlsAlloc(ps3_usleep_destroy);
    return TRUE;
}
static int ps3_usleep_high_resolution(uint64_t usec)
{
    if (!usec || usec > INT64_MAX/10) return 0;
    InitOnceExecuteOnce(&ps3_usleep_once,ps3_usleep_initialize,NULL,NULL);
    if (ps3_usleep_slot == FLS_OUT_OF_INDEXES) return 0;
    HANDLE timer = (HANDLE)FlsGetValue(ps3_usleep_slot);
    if (!timer) {
        /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, supported since Win10 1803. */
        timer = CreateWaitableTimerExW(NULL,NULL,0x2,TIMER_ALL_ACCESS);
        if (!timer) return 0;
        if (!FlsSetValue(ps3_usleep_slot,timer)) { CloseHandle(timer); return 0; }
    }
    LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(usec*10);
    if (!SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE)) return 0;
    return WaitForSingleObject(timer,INFINITE)==WAIT_OBJECT_0;
}
#endif
#endif
