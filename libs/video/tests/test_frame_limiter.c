#include "../rsx_frame_limiter.h"
#include <assert.h>
#include <stdio.h>
#pragma comment(lib, "winmm.lib") /* timeBeginPeriod, as the game process uses. */

static LONGLONG timestamp(void)
{
    LARGE_INTEGER value;
    assert(QueryPerformanceCounter(&value));
    return value.QuadPart;
}

static void busy(LONGLONG ticks)
{
    const LONGLONG end = timestamp() + ticks;
    while (timestamp() < end) YieldProcessor();
}

/* Release no earlier than the scheduled deadline; on-time releases advance the
 * deadline by exactly one period regardless of wake latency or Present cost.
 * Returns the number of frames that kept the grid. */
static unsigned verify_grid(rsx_frame_limiter* limiter, unsigned count, LONGLONG present_cost)
{
    unsigned kept = 0;
    for (unsigned i = 0; i < count; ++i) {
        const LONGLONG deadline = limiter->next_deadline;
        assert(rsx_frame_limiter_wait(limiter) >= 0);
        const LONGLONG released = timestamp();
        assert(!deadline || released >= deadline);
        if (deadline && limiter->next_deadline == deadline + limiter->period) ++kept;
        busy(present_cost); /* Present() and other post-wait work. */
        limiter->last_present = timestamp();
    }
    return kept;
}

int main(void)
{
    LARGE_INTEGER frequency;
    assert(QueryPerformanceFrequency(&frequency));
    timeBeginPeriod(1);
    rsx_frame_limiter limiter;
    rsx_frame_limiter_init(&limiter, frequency.QuadPart, 0);
    assert(!limiter.period && !limiter.timer && limiter.frequency == frequency.QuadPart);
    assert(rsx_frame_limiter_wait(&limiter) == 0);
    rsx_frame_limiter_destroy(&limiter);

    rsx_frame_limiter_init(&limiter, frequency.QuadPart, 60);
    assert(limiter.period * 60 >= frequency.QuadPart && limiter.timer);
    assert(rsx_frame_limiter_wait(&limiter) == 0); // First frame has no predecessor.
    limiter.last_present = timestamp();

    /* Present cost and wake latency must not extend the mean interval. The old
     * last_present+period policy averaged period+cost (about 58 FPS at 60). */
    const LONGLONG present_cost = frequency.QuadPart * 7 / 10000; /* 0.7 ms */
    verify_grid(&limiter, 1, present_cost); /* Anchors the grid. */
    const LONGLONG begin = limiter.last_present;
    const unsigned frames = 120;
    const unsigned kept = verify_grid(&limiter, frames, present_cost);
    const double mean_ms = (limiter.last_present - begin) * 1000.0 / frames / frequency.QuadPart;
    const double period_ms = limiter.period * 1000.0 / frequency.QuadPart;
    printf("60 FPS limiter: %u/%u on grid, mean interval %.4f ms (period %.4f ms)\n",
        kept, frames, mean_ms, period_ms);
    /* Preemption on a loaded machine can re-anchor a few frames; the mean must
     * still stay far below the old period+present_cost result. */
    assert(kept >= frames * 9 / 10);
    assert(mean_ms < period_ms + 0.25);

    /* A small miss (<= period/8) keeps the grid: the next frame is shortened
     * by the miss rather than every later frame shifting. */
    LONGLONG deadline = timestamp() - limiter.period / 16;
    limiter.next_deadline = deadline;
    assert(rsx_frame_limiter_wait(&limiter) == 0);
    assert(limiter.next_deadline == deadline + limiter.period);

    /* A large miss re-anchors to the late release: no catch-up burst. */
    deadline = timestamp() - limiter.period * 3;
    limiter.next_deadline = deadline;
    const LONGLONG before = timestamp();
    assert(rsx_frame_limiter_wait(&limiter) == 0);
    assert(limiter.next_deadline >= before + limiter.period);

    /* Live changes zero last_present or change period; both restart. */
    limiter.last_present = 0;
    assert(rsx_frame_limiter_wait(&limiter) == 0 && limiter.next_deadline == 0);
    limiter.last_present = timestamp();
    limiter.period = (frequency.QuadPart + 29) / 30;
    assert(rsx_frame_limiter_wait(&limiter) > 0);
    assert(limiter.scheduled_period == limiter.period);
    rsx_frame_limiter_destroy(&limiter);
    assert(!limiter.timer && !limiter.period && !limiter.last_present && !limiter.next_deadline);

    /* Starting uncapped and selecting a live cap opens the timer on demand
     * instead of falling back to millisecond Sleep(). */
    rsx_frame_limiter_init(&limiter, frequency.QuadPart, 0);
    limiter.period = (frequency.QuadPart + 59) / 60;
    limiter.last_present = timestamp();
    assert(rsx_frame_limiter_wait(&limiter) > 0 && limiter.timer && limiter.timer_attempted);
    if (limiter.timer) CloseHandle(limiter.timer);
    limiter.timer = NULL;
    /* Timer allocation/error fallback uses Sleep plus the final spin. */
    limiter.last_present = timestamp();
    verify_grid(&limiter, 3, 0);
    assert(!limiter.timer);
    rsx_frame_limiter_destroy(&limiter);
    timeEndPeriod(1);
    puts("Frame limiter: disabled, grid, small/large miss, live restart, lazy timer and fallback checks passed");
    return 0;
}
