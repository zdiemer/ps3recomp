/* Win32 concurrency regression linked to the production coherence lock.
 *
 * An owner deliberately holds the real lock while gated contenders approach
 * it. No contender may publish until release. Repeated contended acquisitions
 * then verify exclusive ownership, complete payload publication and bounded
 * completion. This checks semantics, not fairness or a CPU-time threshold.
 *
 * From the SDK root, in a Visual Studio x64 developer environment:
 *   clang-cl /nologo /TC /O2 /std:c11 /clang:-msse4.1 /Iinclude ^
 *     runtime/spu/tests/test_coherency_lock.c runtime/spu/spu_coherency.c ^
 *     /Fe:test_coherency_lock.exe
 *   test_coherency_lock.exe
 */

/* Checks and synchronization calls must execute even in a Release build. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../spu_coherency.h"
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

enum { CONTENDERS = 8, PASSES = 2000, PAYLOAD_WORDS = 32, DEADLINE_MS = 10000 };

/* Unrelated coherence notifications are not exercised in this lock fixture. */
void spu_ch_wake(spu_context* ctx) { (void)ctx; }

static HANDLE start_gate;
static volatile LONG owners, publications;
static struct {
    uint64_t generation;
    uint64_t words[PAYLOAD_WORDS];
    uint64_t checksum;
} payload;
static unsigned completed[CONTENDERS]; /* protected by the production lock */

typedef struct contender_args {
    unsigned index;
    HANDLE approaching_lock;
} contender_args;

static uint64_t word_value(uint64_t generation, unsigned index)
{
    return (generation * UINT64_C(0x9E3779B97F4A7C15)) ^
           (UINT64_C(0xD1B54A32D192ED03) * (index + 1));
}

static void enter_owned(void)
{
    assert(InterlockedIncrement(&owners) == 1);
}

static void leave_owned(void)
{
    assert(InterlockedDecrement(&owners) == 0);
}

static void check_payload(void)
{
    uint64_t checksum = 0;
    for (unsigned i = 0; i < PAYLOAD_WORDS; ++i) {
        assert(payload.words[i] == word_value(payload.generation, i));
        checksum += payload.words[i];
    }
    assert(checksum == payload.checksum);
}

static void publish_payload(uint64_t generation)
{
    uint64_t checksum = 0;
    payload.generation = generation;
    for (unsigned i = 0; i < PAYLOAD_WORDS; ++i) {
        payload.words[i] = word_value(generation, i);
        checksum += payload.words[i];
        /* Widen the partial-publication interval without another lock/wait. */
        YieldProcessor();
    }
    payload.checksum = checksum;
}

static DWORD WINAPI contender(void* context)
{
    contender_args* args = (contender_args*)context;
    assert(WaitForSingleObject(start_gate, DEADLINE_MS) == WAIT_OBJECT_0);
    assert(SetEvent(args->approaching_lock));
    for (unsigned i = 0; i < PASSES; ++i) {
        spu_lockline_lock();
        enter_owned();
        check_payload();
        publish_payload(payload.generation + 1);
        ++completed[args->index];
        InterlockedIncrement(&publications);
        leave_owned();
        spu_lockline_unlock();
        /* Give peers opportunities without imposing any acquisition order. */
        if ((i & 31u) == 0) SwitchToThread();
    }
    return 0;
}

int main(void)
{
    HANDLE threads[CONTENDERS], approaching[CONTENDERS];
    contender_args args[CONTENDERS];
    start_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(start_gate);

    spu_lockline_lock();
    enter_owned();
    publish_payload(1);
    for (unsigned i = 0; i < CONTENDERS; ++i) {
        approaching[i] = CreateEventA(NULL, TRUE, FALSE, NULL);
        assert(approaching[i]);
        args[i].index = i;
        args[i].approaching_lock = approaching[i];
        threads[i] = CreateThread(NULL, 0, contender, &args[i], 0, NULL);
        assert(threads[i]);
    }
    assert(SetEvent(start_gate));
    assert(WaitForMultipleObjects(CONTENDERS, approaching, TRUE, DEADLINE_MS) == WAIT_OBJECT_0);
    /* Started events precede acquisition; exclusion must hold throughout the
     * owner's deliberate hold, regardless of contender scheduling. */
    assert(WaitForMultipleObjects(CONTENDERS, threads, FALSE, 25) == WAIT_TIMEOUT);
    assert(InterlockedCompareExchange(&publications, 0, 0) == 0);
    assert(InterlockedCompareExchange(&owners, 0, 0) == 1);
    check_payload();
    leave_owned();
    spu_lockline_unlock();

    assert(WaitForMultipleObjects(CONTENDERS, threads, TRUE, DEADLINE_MS) == WAIT_OBJECT_0);
    spu_lockline_lock();
    enter_owned();
    check_payload();
    assert(payload.generation == UINT64_C(1) + CONTENDERS * PASSES);
    assert(InterlockedCompareExchange(&publications, 0, 0) == CONTENDERS * PASSES);
    for (unsigned i = 0; i < CONTENDERS; ++i) {
        DWORD exit_code;
        assert(completed[i] == PASSES);
        assert(GetExitCodeThread(threads[i], &exit_code) && exit_code == 0);
    }
    leave_owned();
    spu_lockline_unlock();
    assert(InterlockedCompareExchange(&owners, 0, 0) == 0);

    for (unsigned i = 0; i < CONTENDERS; ++i) {
        CloseHandle(threads[i]);
        CloseHandle(approaching[i]);
    }
    CloseHandle(start_gate);
    puts("SPU coherence lock: held-owner exclusion, payload publication and contended completion passed");
    return 0;
}
