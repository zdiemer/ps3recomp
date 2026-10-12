/* Opt-in native wake experiment. Queue predicates remain under the SPU lock. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#pragma comment(lib, "synchronization.lib")
int spu_coh_watch_queue(uint32_t ea, void* host_address);
void spu_coh_unwatch_queue(int cookie);

static int queue_native_wait_enabled(void)
{
    static _Thread_local int initialized, enabled, configured;
    static _Thread_local const char* control;
    static _Thread_local ULONGLONG next_check;
    static _Thread_local int reported = -1;
    if (!initialized) {
        initialized = 1;
        const char* setting = getenv("SPURS_QUEUE_WAIT_NATIVE");
        enabled = setting && atoi(setting) != 0;
        control = getenv("SPURS_QUEUE_WAIT_CONTROL");
        configured = setting != NULL || control != NULL;
    }
    if (control && GetTickCount64() >= next_check) {
        next_check = GetTickCount64() + 100;
        FILE* file = fopen(control, "r");
        unsigned value;
        if (file) {
            if (fscanf(file, "%u", &value) == 1) enabled = value != 0;
            fclose(file);
        }
    }
    if (reported != enabled) {
        if (configured) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            fprintf(stderr, "[spurs-queue-wait-mode] tid=%lu native=%d qpc=%lld\n",
                    GetCurrentThreadId(), enabled, now.QuadPart);
        }
        reported = enabled;
    }
    return enabled;
}
static void queue_wait_for_publication(u32 q, u64 snapshot, int native_wait)
{
    if (native_wait) {
        /* A changed header returns immediately, including a publication that
         * raced the unlock. Spurious wakes always recheck the real predicate.
         * The bounded fallback covers writers without coherence notification
         * and watched-line hash collisions; no result is invented or skipped. */
        if (WaitOnAddress(vm_base + q, &snapshot, sizeof(snapshot), 1) ||
            GetLastError() == ERROR_TIMEOUT) return;
    }
    Sleep(1);
}
#else
#define queue_native_wait_enabled() 0
#define spu_coh_watch_queue(ea, host_address) (-1)
#define spu_coh_unwatch_queue(cookie) ((void)0)
#define queue_wait_for_publication(q, snapshot, native_wait) Sleep(1)
#endif
