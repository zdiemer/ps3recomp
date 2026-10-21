#ifndef PS3RECOMP_RSX_FRAME_LIMITER_H
#define PS3RECOMP_RSX_FRAME_LIMITER_H

/* The renderer serializes access and calls rsx_frame_limiter_wait() just
 * before Present. Release deadlines advance on a fixed period grid rather than
 * from the previous present, so timer wake latency and Present() cost do not
 * accumulate into every interval (a 60 FPS cap previously averaged ~58).
 * A frame that misses its deadline by at most period/8 keeps the grid; a
 * later frame re-anchors to its own release, so stalls never cause a
 * catch-up burst. The renderer stores last_present after each successful
 * Present; zeroing it (or changing period) restarts the schedule. */
#include <windows.h>
#include <string.h>

typedef struct rsx_frame_limiter {
    LONGLONG frequency, period, last_present;
    LONGLONG next_deadline, scheduled_period;
    HANDLE timer;
    int high_resolution, timer_attempted;
} rsx_frame_limiter;

static inline void rsx_frame_limiter_open_timer(rsx_frame_limiter* limiter)
{
    limiter->timer_attempted = 1;
    limiter->timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002,
        TIMER_MODIFY_STATE | SYNCHRONIZE); /* HIGH_RESOLUTION, Windows 10 1803+ */
    limiter->high_resolution = limiter->timer != NULL;
    if (!limiter->timer)
        limiter->timer = CreateWaitableTimerW(NULL, FALSE, NULL);
}

static inline void rsx_frame_limiter_init(rsx_frame_limiter* limiter,
    LONGLONG frequency, unsigned fps)
{
    memset(limiter, 0, sizeof(*limiter));
    if (frequency <= 0) return;
    /* Keep the frequency so a later live cap can open the timer on demand. */
    limiter->frequency = frequency;
    if (!fps || fps > 240) return;
    limiter->period = (frequency + fps - 1) / fps;
    rsx_frame_limiter_open_timer(limiter);
}

/* Sleep until shortly before the deadline, then spin the remainder. High
 * resolution timers usually wake within ~0.5 ms, so the spin is bounded by
 * about one millisecond and only runs when the frame is already early. */
static inline void rsx_frame_limiter_sleep_until(rsx_frame_limiter* limiter,
    LONGLONG deadline, LARGE_INTEGER* now)
{
    const LONGLONG margin = limiter->frequency / 1000;
    while (deadline - now->QuadPart > margin) {
        const LONGLONG remaining = deadline - now->QuadPart - margin;
        LARGE_INTEGER due;
        due.QuadPart = -((remaining * 10000000) / limiter->frequency);
        if (!due.QuadPart) break;
        if (!limiter->timer || !SetWaitableTimer(limiter->timer, &due, 0, NULL, NULL, FALSE) ||
            WaitForSingleObject(limiter->timer, INFINITE) != WAIT_OBJECT_0) {
            const DWORD ms = (DWORD)((remaining * 1000) / limiter->frequency);
            if (!ms) break;
            Sleep(ms);
        }
        if (!QueryPerformanceCounter(now)) return;
    }
    while (now->QuadPart < deadline) {
        YieldProcessor();
        if (!QueryPerformanceCounter(now)) return;
    }
}

static inline LONGLONG rsx_frame_limiter_wait(rsx_frame_limiter* limiter)
{
    if (!limiter->period || limiter->frequency <= 0) {
        limiter->next_deadline = 0;
        return 0;
    }
    if (!limiter->last_present) {
        /* First frame after start or a live change has no predecessor. */
        limiter->next_deadline = 0;
        return 0;
    }
    if (!limiter->timer && !limiter->timer_attempted)
        rsx_frame_limiter_open_timer(limiter);
    if (!limiter->next_deadline || limiter->scheduled_period != limiter->period) {
        limiter->next_deadline = limiter->last_present + limiter->period;
        limiter->scheduled_period = limiter->period;
    }
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) return 0;
    const LONGLONG deadline = limiter->next_deadline;
    if (now.QuadPart >= deadline) {
        limiter->next_deadline = now.QuadPart - deadline > limiter->period / 8
            ? now.QuadPart + limiter->period : deadline + limiter->period;
        return 0;
    }
    const LONGLONG begin = now.QuadPart;
    rsx_frame_limiter_sleep_until(limiter, deadline, &now);
    limiter->next_deadline = deadline + limiter->period;
    return now.QuadPart - begin;
}

static inline void rsx_frame_limiter_destroy(rsx_frame_limiter* limiter)
{
    if (limiter->timer) CloseHandle(limiter->timer);
    memset(limiter, 0, sizeof(*limiter));
}
#endif
