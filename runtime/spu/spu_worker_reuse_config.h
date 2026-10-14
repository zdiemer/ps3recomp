/* Internal A/B control. No change to the title's graphics/settings UI. */
#ifndef SPU_WORKER_REUSE_CONFIG_H
#define SPU_WORKER_REUSE_CONFIG_H
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static INIT_ONCE s_worker_config_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK s_worker_config_lock = SRWLOCK_INIT;
static const char* s_worker_config_file;
static volatile LONG s_worker_config_enabled;
static ULONGLONG s_worker_config_next;
static void spu_worker_config_report(int enabled)
{
    LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
    fprintf(stderr, "[spu-worker-reuse-mode] enabled=%d qpc=%lld\n", enabled, qpc.QuadPart);
}
static BOOL CALLBACK spu_worker_config_init(PINIT_ONCE once, PVOID param, PVOID* context)
{
    (void)once; (void)param; (void)context;
    const char* value = getenv("SPU_WORKER_REUSE");
    s_worker_config_file = getenv("SPU_WORKER_REUSE_FILE");
    s_worker_config_enabled = value && strcmp(value, "1") == 0;
    spu_worker_config_report((int)s_worker_config_enabled);
    return TRUE;
}
static int spu_worker_reuse_enabled(void)
{
    InitOnceExecuteOnce(&s_worker_config_once, spu_worker_config_init, NULL, NULL);
    if (s_worker_config_file) {
        AcquireSRWLockExclusive(&s_worker_config_lock);
        ULONGLONG now = GetTickCount64();
        if (now >= s_worker_config_next) {
            s_worker_config_next = now + 100;
            FILE* file = fopen(s_worker_config_file, "r"); unsigned value;
            if (file) {
                if (fscanf(file, "%u", &value) == 1 && value <= 1 &&
                    InterlockedExchange(&s_worker_config_enabled, (LONG)value) != (LONG)value)
                    spu_worker_config_report((int)value);
                fclose(file);
            }
        }
        ReleaseSRWLockExclusive(&s_worker_config_lock);
    }
    return (int)InterlockedCompareExchange(&s_worker_config_enabled, 0, 0);
}
#endif
