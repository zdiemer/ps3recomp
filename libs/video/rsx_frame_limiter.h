#ifndef PS3RECOMP_RSX_FRAME_LIMITER_H
#define PS3RECOMP_RSX_FRAME_LIMITER_H

/* The renderer serializes access. Deadlines follow successful presents, so
 * a late frame never causes a catch-up burst or another full-frame delay. */
#include <windows.h>
#include <string.h>

typedef struct rsx_frame_limiter {
    LONGLONG frequency, period, last_present;
    HANDLE timer;
    int high_resolution;
} rsx_frame_limiter;

static inline void rsx_frame_limiter_init(rsx_frame_limiter* limiter,
    LONGLONG frequency, unsigned fps)
{
    memset(limiter, 0, sizeof(*limiter));
    if (frequency <= 0 || !fps || fps > 240) return;
    limiter->frequency = frequency;
    limiter->period = (frequency + fps - 1) / fps;
    limiter->timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002,
        TIMER_MODIFY_STATE | SYNCHRONIZE); /* HIGH_RESOLUTION, Windows 10 1803+ */
    limiter->high_resolution = limiter->timer != NULL;
    if (!limiter->timer)
        limiter->timer = CreateWaitableTimerW(NULL, FALSE, NULL);
}

static inline LONGLONG rsx_frame_limiter_wait(rsx_frame_limiter* limiter)
{
    if (!limiter->period || !limiter->last_present) return 0;
    const LONGLONG deadline = limiter->last_present + limiter->period;
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now) || now.QuadPart >= deadline) return 0;
    const LONGLONG begin = now.QuadPart;
    do {
        const LONGLONG remaining = deadline - now.QuadPart;
        LARGE_INTEGER due;
        due.QuadPart = -((remaining * 10000000 + limiter->frequency - 1) / limiter->frequency);
        if (!limiter->timer || !SetWaitableTimer(limiter->timer, &due, 0, NULL, NULL, FALSE) ||
            WaitForSingleObject(limiter->timer, INFINITE) != WAIT_OBJECT_0)
            Sleep((DWORD)((remaining * 1000 + limiter->frequency - 1) / limiter->frequency));
        if (!QueryPerformanceCounter(&now)) return 0;
    } while (now.QuadPart < deadline);
    return now.QuadPart - begin;
}

static inline void rsx_frame_limiter_destroy(rsx_frame_limiter* limiter)
{
    if (limiter->timer) CloseHandle(limiter->timer);
    memset(limiter, 0, sizeof(*limiter));
}
#endif
