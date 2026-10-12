/* Native host wait for the real consumer acknowledgment. Opt-in experiment. */
#pragma once
#ifdef _WIN32
#pragma comment(lib, "synchronization.lib")
#endif

static int gcm_fifo_native_wait_enabled(void)
{
#ifdef _WIN32
    static _Thread_local int initialized, enabled, configured;
    static _Thread_local const char* control;
    static _Thread_local ULONGLONG next_check;
    static _Thread_local int reported = -1;
    if (!initialized) {
        initialized = 1;
        const char* setting = getenv("GCM_FIFO_WAIT_NATIVE");
        enabled = setting && atoi(setting) != 0;
        control = getenv("GCM_FIFO_WAIT_CONTROL");
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
            fprintf(stderr, "[gcm-fifo-wait-mode] tid=%lu native=%d qpc=%lld\n",
                    GetCurrentThreadId(), enabled, now.QuadPart);
        }
        reported = enabled;
    }
    return enabled;
#else
    return 0;
#endif
}

static void gcm_fifo_publish_drained_ea(u32 next)
{
    /* Publication describes commands already consumed, never anticipated GET.
     * Always wake on a change, including switches racing the first wait. The
     * address comparison in WaitOnAddress closes publish-before-wait races. */
    u32 previous = atomic_exchange_explicit(&g_gcm_fifo_drained_ea, next,
                                            memory_order_release);
#ifdef _WIN32
    if (previous != next) WakeByAddressAll((void*)&g_gcm_fifo_drained_ea);
#else
    (void)previous;
#endif
}

static void gcm_fifo_wait_for_publication(u32 snapshot, int native_wait)
{
#ifdef _WIN32
    if (native_wait && (WaitOnAddress(&g_gcm_fifo_drained_ea, &snapshot,
                                     sizeof(snapshot), 1) ||
                        GetLastError() == ERROR_TIMEOUT)) return;
#else
    (void)snapshot; (void)native_wait;
#endif
    Sleep(1);
}
