/* Optional legacy shader persistence. The renderer owns start/stop; enqueue
 * may race stop. Jobs contain no renderer, COM, or borrowed source pointers.
 * Keep this coordinator alive until its worker joins (including stop timeout).
 * No concurrent start/stop calls, and quiesce producers before restarting. */
#ifndef RSX_SHADER_STORE_H
#define RSX_SHADER_STORE_H
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define RSX_SHADER_STORE_JOBS 32u
#define RSX_SHADER_STORE_BYTES (16u * 1024u * 1024u)
#define RSX_SHADER_STORE_MAX_SOURCE (256u * 1024u)
#define RSX_SHADER_STORE_MAX_BLOB (4u * 1024u * 1024u)

typedef struct rsx_shader_store_header {
    uint32_t magic, version, stage, source_length, blob_length, reserved;
    uint64_t source_hash;
} rsx_shader_store_header;

typedef struct rsx_shader_store_job {
    struct rsx_shader_store_job* next;
    size_t allocation;
    char path[MAX_PATH], temporary[MAX_PATH];
    rsx_shader_store_header header;
    /* Source then bytecode follow this structure in the same allocation. */
} rsx_shader_store_job;

typedef struct rsx_shader_store_stats {
    uint64_t accepted, written[2], failed, dropped, write_qpc;
    size_t bytes, peak_bytes;
    unsigned jobs, peak_jobs;
} rsx_shader_store_stats;

typedef struct rsx_shader_store {
    SRWLOCK lock;
    CONDITION_VARIABLE changed;
    HANDLE thread;
    int accepting;
    rsx_shader_store_job *head, *tail;
    rsx_shader_store_stats stats; /* includes reserved, queued and active jobs */
} rsx_shader_store;

#ifndef RSX_SHADER_STORE_ALLOC
#define RSX_SHADER_STORE_ALLOC malloc
#endif
#ifndef RSX_SHADER_STORE_BEGIN_THREAD
#define RSX_SHADER_STORE_BEGIN_THREAD _beginthreadex
#endif
#ifndef RSX_SHADER_STORE_IO_WRITE
#define RSX_SHADER_STORE_IO_WRITE WriteFile
#endif
#ifndef RSX_SHADER_STORE_IO_FLUSH
#define RSX_SHADER_STORE_IO_FLUSH FlushFileBuffers
#endif

static int rsx_shader_store_write(const rsx_shader_store_job* job)
{
    HANDLE file = CreateFileA(job->temporary, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    const unsigned char* payload = (const unsigned char*)(job + 1);
    const void* parts[3] = { &job->header, payload,
        payload + job->header.source_length };
    const DWORD lengths[3] = { sizeof(job->header), job->header.source_length,
        job->header.blob_length };
    int success = file != INVALID_HANDLE_VALUE;
    unsigned i;
    if (!success) return 0;
    for (i = 0; success && i < 3; ++i) {
        DWORD offset = 0;
        while (offset < lengths[i]) {
            DWORD wrote = 0;
            if (!RSX_SHADER_STORE_IO_WRITE(file,
                    (const unsigned char*)parts[i] + offset,
                    lengths[i] - offset, &wrote, NULL) || !wrote ||
                    wrote > lengths[i] - offset) {
                success = 0;
                break;
            }
            offset += wrote;
        }
    }
    if (success && !RSX_SHADER_STORE_IO_FLUSH(file)) success = 0;
    if (!CloseHandle(file)) success = 0;
    if (success && !MoveFileExA(job->temporary, job->path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) success = 0;
    if (!success) DeleteFileA(job->temporary);
    return success;
}

#ifndef RSX_SHADER_STORE_WRITE
#define RSX_SHADER_STORE_WRITE rsx_shader_store_write
#endif

static void rsx_shader_store_release_locked(rsx_shader_store* store,
    size_t allocation)
{
    --store->stats.jobs;
    store->stats.bytes -= allocation;
}

static unsigned __stdcall rsx_shader_store_worker(void* argument)
{
    rsx_shader_store* store = (rsx_shader_store*)argument;
    for (;;) {
        rsx_shader_store_job* job;
        LARGE_INTEGER begin = {0}, end = {0};
        int success, begin_valid, end_valid;
        AcquireSRWLockExclusive(&store->lock);
        while (!store->head && store->accepting)
            SleepConditionVariableSRW(&store->changed, &store->lock, INFINITE, 0);
        job = store->head;
        if (!job) {
            ReleaseSRWLockExclusive(&store->lock);
            return 0;
        }
        store->head = job->next;
        if (!store->head) store->tail = NULL;
        ReleaseSRWLockExclusive(&store->lock);
        begin_valid = QueryPerformanceCounter(&begin);
        success = RSX_SHADER_STORE_WRITE(job);
        end_valid = QueryPerformanceCounter(&end);
        AcquireSRWLockExclusive(&store->lock);
        if (success) ++store->stats.written[job->header.stage == 'V' ? 0 : 1];
        else ++store->stats.failed;
        if (begin_valid && end_valid && end.QuadPart >= begin.QuadPart)
            store->stats.write_qpc += (uint64_t)(end.QuadPart - begin.QuadPart);
        rsx_shader_store_release_locked(store, job->allocation);
        ReleaseSRWLockExclusive(&store->lock);
        free(job);
    }
}

/* Only the renderer owner starts/restarts. A timed-out old worker prevents
 * restarting until it exits and all producer reservations have been released. */
static int rsx_shader_store_start(rsx_shader_store* store)
{
    AcquireSRWLockExclusive(&store->lock);
    if (store->thread) {
        if (store->accepting) {
            ReleaseSRWLockExclusive(&store->lock);
            return 1;
        }
        if (WaitForSingleObject(store->thread, 0) != WAIT_OBJECT_0 ||
                store->stats.jobs) {
            ReleaseSRWLockExclusive(&store->lock);
            return 0;
        }
        CloseHandle(store->thread);
        store->thread = NULL;
    }
    memset(&store->stats, 0, sizeof(store->stats));
    store->thread = (HANDLE)RSX_SHADER_STORE_BEGIN_THREAD(
        NULL, 0, rsx_shader_store_worker, store, 0, NULL);
    store->accepting = store->thread != NULL;
    ReleaseSRWLockExclusive(&store->lock);
    return store->accepting;
}

static int rsx_shader_store_enqueue(rsx_shader_store* store, const char* path,
    const rsx_shader_store_header* header, const void* source, const void* blob)
{
    rsx_shader_store_job* job;
    size_t allocation;
    int temporary_length;
    if (!path || !header || !source || !blob ||
            (header->stage != 'V' && header->stage != 'P') ||
            header->source_length > RSX_SHADER_STORE_MAX_SOURCE ||
            !header->blob_length || header->blob_length > RSX_SHADER_STORE_MAX_BLOB ||
            strlen(path) >= MAX_PATH) return 0;
    allocation = sizeof(*job) + (size_t)header->source_length + header->blob_length;
    AcquireSRWLockExclusive(&store->lock);
    if (!store->accepting || store->stats.jobs >= RSX_SHADER_STORE_JOBS ||
            allocation > RSX_SHADER_STORE_BYTES - store->stats.bytes) {
        ++store->stats.dropped;
        ReleaseSRWLockExclusive(&store->lock);
        return 0;
    }
    ++store->stats.jobs;
    store->stats.bytes += allocation;
    if (store->stats.jobs > store->stats.peak_jobs)
        store->stats.peak_jobs = store->stats.jobs;
    if (store->stats.bytes > store->stats.peak_bytes)
        store->stats.peak_bytes = store->stats.bytes;
    ReleaseSRWLockExclusive(&store->lock);
    job = (rsx_shader_store_job*)RSX_SHADER_STORE_ALLOC(allocation);
    if (job) {
        job->next = NULL;
        job->allocation = allocation;
        job->header = *header;
        memcpy(job->path, path, strlen(path) + 1);
        temporary_length = snprintf(job->temporary, sizeof(job->temporary),
            "%s.tmp.%lu", path, (unsigned long)GetCurrentProcessId());
        if (temporary_length <= 0 || temporary_length >= (int)sizeof(job->temporary)) {
            free(job);
            job = NULL;
        } else {
            memcpy(job + 1, source, header->source_length);
            memcpy((unsigned char*)(job + 1) + header->source_length,
                blob, header->blob_length);
        }
    }
    AcquireSRWLockExclusive(&store->lock);
    if (!job || !store->accepting) {
        rsx_shader_store_release_locked(store, allocation);
        ++store->stats.dropped;
        ReleaseSRWLockExclusive(&store->lock);
        free(job);
        return 0;
    }
    if (store->tail) store->tail->next = job;
    else store->head = job;
    store->tail = job;
    ++store->stats.accepted;
    WakeConditionVariable(&store->changed);
    ReleaseSRWLockExclusive(&store->lock);
    return 1;
}

/* Stop admission and try draining within timeout_ms. On timeout only the
 * active write and producer-owned reservations remain; no thread termination
 * or worker-reachable storage destruction. Call again to observe completion.
 * This is bounded waiting, not permission to unload code with a live worker. */
static int rsx_shader_store_stop(rsx_shader_store* store, unsigned timeout_ms)
{
    HANDLE thread;
    rsx_shader_store_job* discarded = NULL;
    int complete;
    AcquireSRWLockExclusive(&store->lock);
    store->accepting = 0;
    thread = store->thread;
    WakeAllConditionVariable(&store->changed);
    ReleaseSRWLockExclusive(&store->lock);
    complete = !thread || WaitForSingleObject(thread, timeout_ms) == WAIT_OBJECT_0;
    AcquireSRWLockExclusive(&store->lock);
    if (!complete) {
        rsx_shader_store_job* job;
        discarded = store->head;
        store->head = store->tail = NULL;
        for (job = discarded; job; job = job->next) {
            rsx_shader_store_release_locked(store, job->allocation);
            ++store->stats.dropped;
        }
    }
    if (store->stats.jobs) complete = 0;
    ReleaseSRWLockExclusive(&store->lock);
    while (discarded) {
        rsx_shader_store_job* next = discarded->next;
        free(discarded);
        discarded = next;
    }
    return complete;
}

static rsx_shader_store_stats rsx_shader_store_snapshot(rsx_shader_store* store)
{
    rsx_shader_store_stats stats;
    AcquireSRWLockShared(&store->lock);
    stats = store->stats;
    ReleaseSRWLockShared(&store->lock);
    return stats;
}
#endif
