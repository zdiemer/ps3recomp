/* Diagnostic wall-time accounting. Ordinary builds compile all probes out. */
#pragma once
#if defined(_WIN32) && defined(YZ_PERF_PROFILE)
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int initialized, recording;
    const char* directory;
    const char* control;
    ULONGLONG next_check;
    LONGLONG frequency;
    FILE* file;
    unsigned rows;
} queue_profile_stream;

typedef struct {
    LONGLONG begin, lock_ticks, wait_ticks;
    unsigned attempts, busy, unavailable;
    u32 taskset;
} queue_profile_scope;

static _Thread_local queue_profile_stream queue_profile_output;

static LONGLONG queue_profile_clock(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static queue_profile_scope queue_profile_begin(void)
{
    queue_profile_scope scope = {0};
    queue_profile_stream* stream = &queue_profile_output;
    if (!stream->initialized) {
        stream->initialized = 1;
        stream->directory = getenv("SPURS_QUEUE_PROFILE");
        stream->control = getenv("SPURS_QUEUE_PROFILE_CONTROL");
        stream->recording = 1;
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        stream->frequency = frequency.QuadPart;
    }
    if (!stream->directory || !*stream->directory) return scope;
    if (stream->control && GetTickCount64() >= stream->next_check) {
        stream->next_check = GetTickCount64() + 1000;
        FILE* file = fopen(stream->control, "r");
        unsigned value;
        if (file) {
            if (fscanf(file, "%u", &value) == 1) stream->recording = value != 0;
            fclose(file);
        }
    }
    if (stream->recording) scope.begin = queue_profile_clock();
    return scope;
}

static s32 queue_profile_finish(queue_profile_scope* scope, u32 queue,
                                int push, u32 blocking, u32 peek, s32 result, int native_wait)
{
    if (!scope->begin) return result;
    LONGLONG end = queue_profile_clock();
    queue_profile_stream* stream = &queue_profile_output;
    if ((end - scope->begin) * 1000000 < stream->frequency * 200) return result;
    /* File creation and formatting are excluded from the measured scope.
     * These diagnostic runs are not substitutes for ordinary-build benchmarks. */
    if (!stream->file) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/queue-%lu.csv", stream->directory,
                 GetCurrentThreadId());
        stream->file = fopen(path, "wb");
        if (!stream->file) return result;
        setvbuf(stream->file, NULL, _IOFBF, 256 * 1024);
        fprintf(stream->file, "begin_qpc,end_qpc,frequency,host_tid,queue,taskset,push,blocking,peek,result,attempts,busy,unavailable,lock_ticks,wait_ticks,native_wait\n");
    }
    fprintf(stream->file, "%lld,%lld,%lld,%lu,%08X,%08X,%d,%u,%u,%08X,%u,%u,%u,%lld,%lld,%d\n",
            scope->begin, end, stream->frequency, GetCurrentThreadId(), queue,
            scope->taskset, push, blocking, peek, (unsigned)result,
            scope->attempts, scope->busy, scope->unavailable,
            scope->lock_ticks, scope->wait_ticks, native_wait);
    if ((++stream->rows & 63u) == 0) fflush(stream->file);
    return result;
}
#define QUEUE_PROFILE_BEGIN() queue_profile_scope profile = queue_profile_begin()
#define QUEUE_PROFILE_LOCK_BEGIN() LONGLONG lock_begin = profile.begin ? queue_profile_clock() : 0
#define QUEUE_PROFILE_LOCK_END() do { if (profile.begin) { \
    profile.lock_ticks += queue_profile_clock() - lock_begin; ++profile.attempts; \
    } } while (0)
#define QUEUE_PROFILE_WAIT(busy_state) do { if (profile.begin) { \
    if (busy_state) ++profile.busy; else ++profile.unavailable; \
    } } while (0)
#define QUEUE_PROFILE_TASKSET(value) do { if (profile.begin) profile.taskset = (value); } while (0)
#define QUEUE_PROFILE_SLEEP(wait_call) do { \
    LONGLONG sleep_begin = profile.begin ? queue_profile_clock() : 0; \
    wait_call; \
    if (profile.begin) profile.wait_ticks += queue_profile_clock() - sleep_begin; \
    } while (0)
#define QUEUE_RETURN(value) return queue_profile_finish(&profile, q, push, blocking, peek, (value), native_wait)
#else
#define QUEUE_PROFILE_BEGIN() ((void)0)
#define QUEUE_PROFILE_LOCK_BEGIN() ((void)0)
#define QUEUE_PROFILE_LOCK_END() ((void)0)
#define QUEUE_PROFILE_WAIT(busy_state) ((void)0)
#define QUEUE_PROFILE_TASKSET(value) ((void)0)
#define QUEUE_PROFILE_SLEEP(wait_call) wait_call
#define QUEUE_RETURN(value) return (value)
#endif
