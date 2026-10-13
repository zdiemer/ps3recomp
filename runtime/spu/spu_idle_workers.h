/*
 * Small cache of idle Windows workers for short, returning SPU jobs.
 *
 * This header's static state is intentionally one-per-translation-unit. Include
 * it in exactly one implementation TU. There is no work queue and no bound on
 * concurrent execution: a submit reuses an IDLE worker, starts a retained
 * worker if a slot is available, or falls back to an ordinary detached thread
 * when every retained slot is occupied. Thus a blocked worker cannot prevent a
 * producer or dependent job from starting on another thread.
 *
 * The callback must initialize and clean up all per-job state before returning.
 * The cache itself does not inspect SPU state and has no shutdown operation;
 * idle workers retire after SPU_IDLE_WORKER_TIMEOUT_MS. Process teardown while
 * jobs are still running is the host's normal process teardown, not a join.
 *
 * On success, ownership of arg transfers to fn(arg). On failure, fn is not
 * called and ownership remains with the caller. `reused` is set to one only
 * when an already-idle thread accepted the job; newly-created retained and
 * one-shot threads set it to zero.
 */
#ifndef PS3RECOMP_SPU_IDLE_WORKERS_H
#define PS3RECOMP_SPU_IDLE_WORKERS_H

#if defined(_WIN32)

#include <windows.h>
#include <stddef.h>

#ifndef SPU_IDLE_WORKER_SLOTS
#define SPU_IDLE_WORKER_SLOTS 16
#endif

#ifndef SPU_IDLE_WORKER_TIMEOUT_MS
#define SPU_IDLE_WORKER_TIMEOUT_MS 5000
#endif

typedef void (*spu_idle_worker_fn)(void*);

typedef struct spu_idle_worker_call {
    spu_idle_worker_fn fn;
    void* arg;
} spu_idle_worker_call;

enum spu_idle_worker_state {
    SPU_IDLE_SLOT_EMPTY = 0,
    SPU_IDLE_SLOT_STARTING,
    SPU_IDLE_SLOT_ASSIGNED,
    SPU_IDLE_SLOT_BUSY,
    SPU_IDLE_SLOT_IDLE
};

typedef struct spu_idle_worker_slot {
    enum spu_idle_worker_state state;
    CONDITION_VARIABLE wake;
    spu_idle_worker_fn fn;
    void* arg;
} spu_idle_worker_slot;

static SRWLOCK s_spu_idle_workers_lock = SRWLOCK_INIT;
static spu_idle_worker_slot s_spu_idle_workers[SPU_IDLE_WORKER_SLOTS];

#if defined(SPU_IDLE_WORKERS_TESTING)
static volatile LONG s_spu_idle_workers_fail_next_create;
static inline void spu_idle_workers_test_fail_next_create(void)
{
    InterlockedExchange(&s_spu_idle_workers_fail_next_create, 1);
}
static inline int spu_idle_workers_test_consume_create_failure(void)
{
    return InterlockedExchange(&s_spu_idle_workers_fail_next_create, 0) != 0;
}
#else
static inline int spu_idle_workers_test_consume_create_failure(void) { return 0; }
#endif

static DWORD WINAPI spu_idle_worker_main(LPVOID opaque)
{
    spu_idle_worker_slot* slot = (spu_idle_worker_slot*)opaque;

    for (;;) {
        spu_idle_worker_fn fn;
        void* arg;

        AcquireSRWLockExclusive(&s_spu_idle_workers_lock);
        while (slot->state == SPU_IDLE_SLOT_STARTING ||
               slot->state == SPU_IDLE_SLOT_IDLE) {
            const DWORD timeout = slot->state == SPU_IDLE_SLOT_STARTING
                                ? INFINITE : SPU_IDLE_WORKER_TIMEOUT_MS;
            if (SleepConditionVariableSRW(&slot->wake,
                                          &s_spu_idle_workers_lock,
                                          timeout, 0))
                continue;

            if (GetLastError() == ERROR_TIMEOUT &&
                slot->state == SPU_IDLE_SLOT_IDLE) {
                /* Publish the slot as reusable before exiting. The retiring
                 * thread touches no slot memory after dropping the lock. */
                slot->state = SPU_IDLE_SLOT_EMPTY;
                slot->fn = NULL;
                slot->arg = NULL;
                ReleaseSRWLockExclusive(&s_spu_idle_workers_lock);
                return 0;
            }
        }

        if (slot->state != SPU_IDLE_SLOT_ASSIGNED) {
            /* No normal path reaches this branch. Retire defensively without
             * exposing a partially-owned callback. */
            slot->state = SPU_IDLE_SLOT_EMPTY;
            slot->fn = NULL;
            slot->arg = NULL;
            ReleaseSRWLockExclusive(&s_spu_idle_workers_lock);
            return 0;
        }

        fn = slot->fn;
        arg = slot->arg;
        slot->fn = NULL;
        slot->arg = NULL;
        slot->state = SPU_IDLE_SLOT_BUSY;
        ReleaseSRWLockExclusive(&s_spu_idle_workers_lock);

        fn(arg);

        AcquireSRWLockExclusive(&s_spu_idle_workers_lock);
        slot->state = SPU_IDLE_SLOT_IDLE;
        WakeConditionVariable(&slot->wake);
        ReleaseSRWLockExclusive(&s_spu_idle_workers_lock);
    }
}

static DWORD WINAPI spu_idle_worker_one_shot(LPVOID opaque)
{
    spu_idle_worker_call* call = (spu_idle_worker_call*)opaque;
    spu_idle_worker_fn fn = call->fn;
    void* arg = call->arg;
    HeapFree(GetProcessHeap(), 0, opaque);
    fn(arg);
    return 0;
}

static inline int spu_idle_worker_submit(spu_idle_worker_fn fn, void* arg,
                                         int* reused)
{
    if (reused) *reused = 0;
    if (!fn) return 0;

    AcquireSRWLockExclusive(&s_spu_idle_workers_lock);
    for (size_t i = 0; i < SPU_IDLE_WORKER_SLOTS; ++i) {
        spu_idle_worker_slot* slot = &s_spu_idle_workers[i];
        if (slot->state == SPU_IDLE_SLOT_IDLE) {
            slot->fn = fn;
            slot->arg = arg;
            slot->state = SPU_IDLE_SLOT_ASSIGNED;
            WakeConditionVariable(&slot->wake);
            ReleaseSRWLockExclusive(&s_spu_idle_workers_lock);
            if (reused) *reused = 1;
            return 1;
        }
    }

    spu_idle_worker_slot* free_slot = NULL;
    for (size_t i = 0; i < SPU_IDLE_WORKER_SLOTS; ++i) {
        if (s_spu_idle_workers[i].state == SPU_IDLE_SLOT_EMPTY) {
            free_slot = &s_spu_idle_workers[i];
            free_slot->state = SPU_IDLE_SLOT_STARTING;
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_spu_idle_workers_lock);

    if (free_slot) {
        InitializeConditionVariable(&free_slot->wake);
        if (spu_idle_workers_test_consume_create_failure()) {
            AcquireSRWLockExclusive(&s_spu_idle_workers_lock);
            free_slot->state = SPU_IDLE_SLOT_EMPTY;
            ReleaseSRWLockExclusive(&s_spu_idle_workers_lock);
            return 0;
        }
        HANDLE thread = CreateThread(NULL, 256u << 20, spu_idle_worker_main,
                                     free_slot, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                     NULL);
        if (!thread) {
            AcquireSRWLockExclusive(&s_spu_idle_workers_lock);
            free_slot->state = SPU_IDLE_SLOT_EMPTY;
            ReleaseSRWLockExclusive(&s_spu_idle_workers_lock);
            return 0;
        }
        CloseHandle(thread);

        AcquireSRWLockExclusive(&s_spu_idle_workers_lock);
        free_slot->fn = fn;
        free_slot->arg = arg;
        free_slot->state = SPU_IDLE_SLOT_ASSIGNED;
        WakeConditionVariable(&free_slot->wake);
        ReleaseSRWLockExclusive(&s_spu_idle_workers_lock);
        return 1;
    }

    /* All cache slots are active or starting. Preserve unbounded concurrency:
     * spill to the same one-job detached-thread model used before this cache. */
    if (spu_idle_workers_test_consume_create_failure()) return 0;
    spu_idle_worker_call* call = (spu_idle_worker_call*)HeapAlloc(
        GetProcessHeap(), 0, sizeof(*call));
    if (!call) return 0;
    call->fn = fn;
    call->arg = arg;
    HANDLE thread = CreateThread(NULL, 256u << 20, spu_idle_worker_one_shot,
                                 call, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!thread) {
        HeapFree(GetProcessHeap(), 0, call);
        return 0;
    }
    CloseHandle(thread);
    return 1;
}

#endif /* _WIN32 */
#endif /* PS3RECOMP_SPU_IDLE_WORKERS_H */
