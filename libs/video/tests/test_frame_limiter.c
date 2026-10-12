#include "../rsx_frame_limiter.h"
#include <assert.h>
#include <stdio.h>

static LONGLONG timestamp(void)
{
    LARGE_INTEGER value;
    assert(QueryPerformanceCounter(&value));
    return value.QuadPart;
}

static void verify_intervals(rsx_frame_limiter* limiter, unsigned count)
{
    limiter->last_present = timestamp();
    for (unsigned i = 0; i < count; ++i) {
        const LONGLONG previous = limiter->last_present;
        /* Preemption can itself consume the interval before the wait. */
        assert(rsx_frame_limiter_wait(limiter) >= 0);
        limiter->last_present = timestamp();
        assert(limiter->last_present - previous >= limiter->period);
    }
}

int main(void)
{
    LARGE_INTEGER frequency;
    assert(QueryPerformanceFrequency(&frequency));
    rsx_frame_limiter limiter;
    rsx_frame_limiter_init(&limiter, frequency.QuadPart, 0);
    assert(!limiter.period && !limiter.timer);
    assert(rsx_frame_limiter_wait(&limiter) == 0);
    rsx_frame_limiter_destroy(&limiter);

    rsx_frame_limiter_init(&limiter, frequency.QuadPart, 30);
    assert(limiter.period * 30 >= frequency.QuadPart);
    assert(rsx_frame_limiter_wait(&limiter) == 0); // First frame has no predecessor.
    verify_intervals(&limiter, 30);
    /* A slow frame is ready immediately; its successor still waits a full
     * interval from the new present. There is no catch-up burst. */
    limiter.last_present = timestamp() - limiter.period * 3;
    assert(rsx_frame_limiter_wait(&limiter) == 0);
    verify_intervals(&limiter, 2);
    printf("30 FPS limiter: 32 real intervals >= %.6f ms, late frames do not wait\n",
        limiter.period * 1000.0 / frequency.QuadPart);
    if (limiter.timer) CloseHandle(limiter.timer);
    limiter.timer = NULL;
    verify_intervals(&limiter, 3); // Timer allocation/error fallback uses Sleep.
    rsx_frame_limiter_destroy(&limiter);
    assert(!limiter.timer && !limiter.period && !limiter.last_present);
    puts("Frame limiter: disabled, initial, bounded, missed deadline and timer fallback checks passed");
    return 0;
}
