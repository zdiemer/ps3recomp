/* More native task contexts than physical SPUs must still have private DMA. */
#include <assert.h>
#include <stdio.h>
#include "../spu_mfc_state.h"
#include "../../platform/win32_compat.h"
#ifndef _WIN32
#include <pthread.h>
#endif
#define NCTX 32
static spu_context* contexts[NCTX];
static mfc_engine* engines[NCTX];
static volatile LONG arrived;

#ifdef _WIN32
static DWORD WINAPI worker(void* value)
#else
static void* worker(void* value)
#endif
{
    unsigned index = (unsigned)(uintptr_t)value;
    spu_context* ctx = contexts[index];
    mfc_engine* engine = spu_mfc_state_get(ctx);
    assert(engine->tag_completed == UINT32_MAX && engine->queue_count == 0);
    engines[index] = engine;
    engine->tag_completed = 0xA5000000u | index;
    engine->queue_count = index + 1;
    InterlockedIncrement(&arrived);
    ULONGLONG start = GetTickCount64();
    while (InterlockedCompareExchange(&arrived, 0, 0) != NCTX) {
        assert(GetTickCount64() - start < 10000);
        Sleep(0);
    }
    assert(spu_mfc_state_get(ctx) == engine);
    assert(engine->tag_completed == (0xA5000000u | index));
    assert(engine->queue_count == index + 1);
    return 0;
}

int main(void)
{
    for (unsigned i = 0; i < NCTX; ++i) {
        contexts[i] = (spu_context*)calloc(1, sizeof(spu_context));
        assert(contexts[i]);
    }
#ifdef _WIN32
    HANDLE threads[NCTX];
    for (unsigned i = 0; i < NCTX; ++i) {
        threads[i] = CreateThread(NULL, 0, worker, (void*)(uintptr_t)i, 0, NULL);
        assert(threads[i]);
    }
    assert(WaitForMultipleObjects(NCTX, threads, TRUE, 15000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < NCTX; ++i) CloseHandle(threads[i]);
#else
    pthread_t threads[NCTX];
    for (unsigned i = 0; i < NCTX; ++i)
        assert(!pthread_create(&threads[i], NULL, worker, (void*)(uintptr_t)i));
    for (unsigned i = 0; i < NCTX; ++i) assert(!pthread_join(threads[i], NULL));
#endif
    for (unsigned i = 0; i < NCTX; ++i) {
        for (unsigned j = i + 1; j < NCTX; ++j) assert(engines[i] != engines[j]);
        spu_mfc_state_release(contexts[i]);
        assert(contexts[i]->mfc_state == NULL);
        /* Reusing the same context must initialize fresh tags and commands. */
        for (unsigned round = 0; round < 100; ++round) {
            mfc_engine* engine = spu_mfc_state_get(contexts[i]);
            assert(engine->tag_completed == UINT32_MAX && engine->queue_count == 0);
            engine->tag_completed = 0; engine->queue_count = 3;
            spu_mfc_state_release(contexts[i]);
        }
        spu_mfc_state_release(contexts[i]);
        free(contexts[i]);
    }
    spu_mfc_state_release(NULL);
    puts("32 concurrent MFC contexts and 3,200 cleanup/reuse cycles passed");
    return 0;
}
