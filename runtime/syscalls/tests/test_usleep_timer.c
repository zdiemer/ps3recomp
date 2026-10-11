#include <assert.h>
#include <stdio.h>
#include "../../platform/win32_usleep.h"
static DWORD WINAPI worker(void* unused)
{
    (void)unused;
    for (unsigned n=0; n<100; ++n) assert(ps3_usleep_high_resolution(50));
    return 0;
}
int main(void)
{
    assert(!ps3_usleep_high_resolution(0));
    assert(!ps3_usleep_high_resolution(UINT64_MAX));
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    const unsigned requests[]={50,100,330,999,1000,2000};
    for (unsigned r=0; r<sizeof(requests)/sizeof(*requests); ++r) {
        double sum=0,maximum=0;
        for (unsigned n=0; n<50; ++n) {
            LARGE_INTEGER a,b; QueryPerformanceCounter(&a);
            assert(ps3_usleep_high_resolution(requests[r]));
            QueryPerformanceCounter(&b);
            const double elapsed=(b.QuadPart-a.QuadPart)*1000000.0/frequency.QuadPart;
            /* Allow 1 us for independent clock quantization. Scheduling may
             * extend a sleep; it must never deliberately shorten the wait. */
            assert(elapsed+1>=requests[r]);
            sum+=elapsed; if (elapsed>maximum) maximum=elapsed;
        }
        printf("timer request=%u us mean=%.2f us max=%.2f us\n",requests[r],sum/50,maximum);
    }
    DWORD before,after; assert(GetProcessHandleCount(GetCurrentProcess(),&before));
    for (unsigned n=0; n<24; ++n) {
        HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL); assert(thread);
        assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); CloseHandle(thread);
    }
    assert(GetProcessHandleCount(GetCurrentProcess(),&after));
    assert(after==before);
    puts("Reusable high-resolution timer: waits meet requested duration, per-thread handles close");
    return 0;
}
