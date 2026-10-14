/* Actual production queue and writer, with deterministic gates/failures.
 * Standalone (VS developer shell):
 * clang-cl /nologo /TC /W4 /WX /O2 test_shader_store.c /Fe:test_shader_store.exe
 * Run without arguments. All artifacts are removed from a private temp dir. */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <process.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rsx_shader_store_job;
static int test_write(const struct rsx_shader_store_job* job);
static void* test_allocate(size_t size);
static uintptr_t test_begin_thread(void* security, unsigned stack_size,
    unsigned (__stdcall *entry)(void*), void* argument, unsigned flags,
    unsigned* thread_id);
static BOOL test_write_file(HANDLE file, LPCVOID buffer, DWORD size,
    LPDWORD written, LPOVERLAPPED overlapped);
static BOOL test_flush_file(HANDLE file);
#define RSX_SHADER_STORE_WRITE test_write
#define RSX_SHADER_STORE_ALLOC test_allocate
#define RSX_SHADER_STORE_BEGIN_THREAD test_begin_thread
#define RSX_SHADER_STORE_IO_WRITE test_write_file
#define RSX_SHADER_STORE_IO_FLUSH test_flush_file
#include "../rsx_shader_store.h"

static HANDLE write_entered, write_release, allocate_entered, allocate_release;
static volatile LONG block_write, block_allocate, fail_allocate, fail_thread;
static volatile LONG fail_second_write, io_calls;
static volatile LONG fail_flush, flush_calls, short_writes;
static rsx_shader_store store;

static int test_write(const rsx_shader_store_job* job)
{
    if (InterlockedCompareExchange(&block_write, 0, 0)) {
        SetEvent(write_entered);
        assert(WaitForSingleObject(write_release, 5000) == WAIT_OBJECT_0);
    }
    return rsx_shader_store_write(job);
}

static void* test_allocate(size_t size)
{
    if (InterlockedCompareExchange(&block_allocate, 0, 0)) {
        SetEvent(allocate_entered);
        assert(WaitForSingleObject(allocate_release, 5000) == WAIT_OBJECT_0);
    }
    return InterlockedCompareExchange(&fail_allocate, 0, 0) ? NULL : malloc(size);
}

static uintptr_t test_begin_thread(void* security, unsigned stack_size,
    unsigned (__stdcall *entry)(void*), void* argument, unsigned flags,
    unsigned* thread_id)
{
    if (InterlockedCompareExchange(&fail_thread, 0, 0)) return 0;
    return _beginthreadex(security, stack_size, entry, argument, flags, thread_id);
}

static BOOL test_write_file(HANDLE file, LPCVOID buffer, DWORD size,
    LPDWORD written, LPOVERLAPPED overlapped)
{
    if (InterlockedIncrement(&io_calls) == 2 &&
            InterlockedCompareExchange(&fail_second_write, 0, 0)) {
        *written = 0;
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    if (InterlockedCompareExchange(&short_writes, 0, 0) && size > 2)
        size = 2;
    return WriteFile(file, buffer, size, written, overlapped);
}

static BOOL test_flush_file(HANDLE file)
{
    InterlockedIncrement(&flush_calls);
    if (InterlockedCompareExchange(&fail_flush, 0, 0)) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return FlushFileBuffers(file);
}

static rsx_shader_store_header header(unsigned source_size, unsigned blob_size)
{
    rsx_shader_store_header h = {0};
    h.magic = 0x43535A59u;
    h.version = 2;
    h.stage = 'V';
    h.source_length = source_size;
    h.blob_length = blob_size;
    h.source_hash = UINT64_C(0x1122334455667788);
    return h;
}

static void write_gate(void)
{
    ResetEvent(write_entered);
    ResetEvent(write_release);
    InterlockedExchange(&block_write, 1);
}

static void finish_write(void)
{
    InterlockedExchange(&block_write, 0);
    SetEvent(write_release);
    assert(rsx_shader_store_stop(&store, 5000));
}

static void read_exact(const char* path, const void* expected, DWORD size)
{
    unsigned char data[128];
    DWORD got = 0, extra = 0;
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(size <= sizeof(data));
    assert(file != INVALID_HANDLE_VALUE);
    assert(ReadFile(file, data, size, &got, NULL) && got == size);
    assert(memcmp(data, expected, size) == 0);
    assert(ReadFile(file, data, 1, &extra, NULL) && extra == 0);
    assert(CloseHandle(file));
}

typedef struct enqueue_argument {
    const char* path;
    rsx_shader_store_header h;
    int result;
} enqueue_argument;

static unsigned __stdcall enqueue_thread(void* pointer)
{
    enqueue_argument* argument = (enqueue_argument*)pointer;
    argument->result = rsx_shader_store_enqueue(&store, argument->path,
        &argument->h, "abcd", "efgh");
    return 0;
}

int main(void)
{
    char base[MAX_PATH], directory[MAX_PATH], path[MAX_PATH], temporary[MAX_PATH];
    char source[] = "abcd", blob[] = "efgh";
    unsigned char expected[sizeof(rsx_shader_store_header) + 8];
    rsx_shader_store_header h = header(4, 4);
    rsx_shader_store_stats stats;
    unsigned i;
    assert(sizeof(rsx_shader_store_header) == 32); /* legacy ABI */
    assert(GetTempPathA(sizeof(base), base));
    assert(GetTempFileNameA(base, "rsx", 0, directory));
    assert(DeleteFileA(directory));
    assert(CreateDirectoryA(directory, NULL));
    assert(snprintf(path, sizeof(path), "%s\\shader.bin", directory) > 0);
    assert(snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", path,
        (unsigned long)GetCurrentProcessId()) > 0);
    write_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
    write_release = CreateEventA(NULL, TRUE, FALSE, NULL);
    allocate_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
    allocate_release = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(write_entered && write_release && allocate_entered && allocate_release);
    assert(rsx_shader_store_stop(&store, 0));

    /* Start failure is optional, and enqueue cannot fall back to file I/O. */
    InterlockedExchange(&fail_thread, 1);
    assert(!rsx_shader_store_start(&store));
    assert(!rsx_shader_store_enqueue(&store, path, &h, source, blob));
    assert(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
    InterlockedExchange(&fail_thread, 0);
    assert(rsx_shader_store_start(&store));
    InterlockedExchange(&fail_allocate, 1);
    assert(!rsx_shader_store_enqueue(&store, path, &h, source, blob));
    stats = rsx_shader_store_snapshot(&store);
    assert(!stats.jobs && !stats.bytes && stats.dropped == 1);
    InterlockedExchange(&fail_allocate, 0);

    /* Enqueue completed while the actual writer is still gated. Caller
     * storage may change or disappear without changing the saved payload. */
    write_gate();
    memcpy(expected, &h, sizeof(h));
    memcpy(expected + sizeof(h), "abcdefgh", 8);
    assert(rsx_shader_store_enqueue(&store, path, &h, source, blob));
    assert(WaitForSingleObject(write_entered, 5000) == WAIT_OBJECT_0);
    memset(source, 'X', 4);
    memset(blob, 'Y', 4);
    assert(!rsx_shader_store_stop(&store, 0));
    assert(!rsx_shader_store_start(&store));
    assert(!rsx_shader_store_enqueue(&store, path, &h, source, blob));
    stats = rsx_shader_store_snapshot(&store);
    assert(stats.jobs == 1 && stats.written[0] == 0);
    finish_write();
    read_exact(path, expected, sizeof(expected));
    assert(rsx_shader_store_stop(&store, 0));

    /* Both capacity bounds count the active job. Timeout discards pending
     * jobs without touching the blocked writer's allocation. */
    assert(rsx_shader_store_start(&store));
    write_gate();
    assert(rsx_shader_store_enqueue(&store, path, &h, "abcd", "efgh"));
    assert(WaitForSingleObject(write_entered, 5000) == WAIT_OBJECT_0);
    for (i = 1; i < RSX_SHADER_STORE_JOBS; ++i)
        assert(rsx_shader_store_enqueue(&store, path, &h, "abcd", "efgh"));
    assert(!rsx_shader_store_enqueue(&store, path, &h, "abcd", "efgh"));
    stats = rsx_shader_store_snapshot(&store);
    assert(stats.jobs == RSX_SHADER_STORE_JOBS && stats.peak_jobs == RSX_SHADER_STORE_JOBS);
    assert(!rsx_shader_store_stop(&store, 0));
    stats = rsx_shader_store_snapshot(&store);
    assert(stats.jobs == 1 && stats.dropped == RSX_SHADER_STORE_JOBS);
    finish_write();
    {
        unsigned char* large = (unsigned char*)malloc(RSX_SHADER_STORE_MAX_BLOB);
        unsigned char* large_source = (unsigned char*)malloc(RSX_SHADER_STORE_MAX_SOURCE);
        rsx_shader_store_header large_h = header(
            RSX_SHADER_STORE_MAX_SOURCE, RSX_SHADER_STORE_MAX_BLOB);
        assert(large && large_source);
        memset(large, 0x6B, RSX_SHADER_STORE_MAX_BLOB);
        memset(large_source, 0x3A, RSX_SHADER_STORE_MAX_SOURCE);
        assert(rsx_shader_store_start(&store));
        write_gate();
        assert(rsx_shader_store_enqueue(&store, path, &large_h, large_source, large));
        assert(WaitForSingleObject(write_entered, 5000) == WAIT_OBJECT_0);
        assert(rsx_shader_store_enqueue(&store, path, &large_h, large_source, large));
        assert(rsx_shader_store_enqueue(&store, path, &large_h, large_source, large));
        assert(!rsx_shader_store_enqueue(&store, path, &large_h, large_source, large));
        stats = rsx_shader_store_snapshot(&store);
        assert(stats.jobs == 3 && stats.peak_bytes <= RSX_SHADER_STORE_BYTES);
        assert(!rsx_shader_store_stop(&store, 0));
        free(large_source);
        free(large);
        finish_write();
    }

    /* A producer reserved memory but has not published when stop closes
     * admission. It must release its reservation, never revive the queue. */
    {
        enqueue_argument argument = {path, {0}, -1};
        HANDLE producer;
        argument.h = h;
        assert(rsx_shader_store_start(&store));
        ResetEvent(allocate_entered);
        ResetEvent(allocate_release);
        InterlockedExchange(&block_allocate, 1);
        producer = (HANDLE)_beginthreadex(NULL, 0, enqueue_thread, &argument, 0, NULL);
        assert(producer);
        assert(WaitForSingleObject(allocate_entered, 5000) == WAIT_OBJECT_0);
        assert(!rsx_shader_store_stop(&store, 0));
        assert(!rsx_shader_store_start(&store));
        InterlockedExchange(&block_allocate, 0);
        SetEvent(allocate_release);
        assert(WaitForSingleObject(producer, 5000) == WAIT_OBJECT_0);
        CloseHandle(producer);
        assert(argument.result == 0);
        assert(rsx_shader_store_stop(&store, 5000));
        stats = rsx_shader_store_snapshot(&store);
        assert(!stats.jobs && !stats.bytes && !stats.accepted);
    }

    /* Restore a known valid destination; partial write failure must preserve
     * it and remove the temporary. Then verify ordinary draining/restart. */
    assert(rsx_shader_store_start(&store));
    assert(rsx_shader_store_enqueue(&store, path, &h, "abcd", "efgh"));
    assert(rsx_shader_store_stop(&store, 5000));
    read_exact(path, expected, sizeof(expected));
    assert(rsx_shader_store_start(&store));
    InterlockedExchange(&io_calls, 0);
    InterlockedExchange(&fail_second_write, 1);
    assert(rsx_shader_store_enqueue(&store, path, &h, "XXXX", "YYYY"));
    assert(rsx_shader_store_stop(&store, 5000));
    stats = rsx_shader_store_snapshot(&store);
    assert(stats.failed == 1 && !stats.written[0] && !stats.jobs && !stats.bytes);
    read_exact(path, expected, sizeof(expected));
    assert(GetFileAttributesA(temporary) == INVALID_FILE_ATTRIBUTES);
    InterlockedExchange(&fail_second_write, 0);

    /* Even after all bytes were written, a failed durability flush must not
     * replace the valid destination or leave its temporary behind. */
    assert(rsx_shader_store_start(&store));
    InterlockedExchange(&flush_calls, 0);
    InterlockedExchange(&fail_flush, 1);
    assert(rsx_shader_store_enqueue(&store, path, &h, "XXXX", "YYYY"));
    assert(rsx_shader_store_stop(&store, 5000));
    stats = rsx_shader_store_snapshot(&store);
    assert(flush_calls == 1 && stats.failed == 1 && !stats.written[0]);
    assert(!stats.jobs && !stats.bytes);
    read_exact(path, expected, sizeof(expected));
    assert(GetFileAttributesA(temporary) == INVALID_FILE_ATTRIBUTES);
    InterlockedExchange(&fail_flush, 0);

    /* Successful short writes must advance through the header, source and
     * bytecode until every byte is persisted exactly once. */
    assert(rsx_shader_store_start(&store));
    InterlockedExchange(&io_calls, 0);
    InterlockedExchange(&flush_calls, 0);
    InterlockedExchange(&short_writes, 1);
    assert(rsx_shader_store_enqueue(&store, path, &h, "abcd", "efgh"));
    assert(rsx_shader_store_stop(&store, 5000));
    stats = rsx_shader_store_snapshot(&store);
    assert(io_calls == (LONG)(sizeof(h) + 8) / 2 && flush_calls == 1);
    assert(stats.written[0] == 1 && !stats.failed && !stats.jobs && !stats.bytes);
    read_exact(path, expected, sizeof(expected));
    assert(GetFileAttributesA(temporary) == INVALID_FILE_ATTRIBUTES);
    InterlockedExchange(&short_writes, 0);

    assert(DeleteFileA(path));
    assert(CreateDirectoryA(path, NULL)); /* rename over a directory fails */
    assert(rsx_shader_store_start(&store));
    assert(rsx_shader_store_enqueue(&store, path, &h, "abcd", "efgh"));
    assert(rsx_shader_store_stop(&store, 5000));
    assert(rsx_shader_store_snapshot(&store).failed == 1);
    assert(GetFileAttributesA(path) & FILE_ATTRIBUTE_DIRECTORY);
    assert(GetFileAttributesA(temporary) == INVALID_FILE_ATTRIBUTES);
    assert(RemoveDirectoryA(path));

    /* Successful stop drains the FIFO, preserving the last exact payload.
     * Both shader stages use the unchanged on-disk header. */
    assert(rsx_shader_store_start(&store));
    for (i = 0; i < RSX_SHADER_STORE_JOBS; ++i) {
        rsx_shader_store_header next = h;
        next.stage = i & 1 ? 'P' : 'V';
        next.source_hash = i;
        assert(rsx_shader_store_enqueue(&store, path, &next, "last", "blob"));
        if (i == RSX_SHADER_STORE_JOBS - 1) {
            memcpy(expected, &next, sizeof(next));
            memcpy(expected + sizeof(next), "lastblob", 8);
        }
    }
    assert(rsx_shader_store_stop(&store, 5000));
    stats = rsx_shader_store_snapshot(&store);
    assert(stats.accepted == RSX_SHADER_STORE_JOBS && !stats.failed && !stats.dropped);
    assert(stats.written[0] == 16 && stats.written[1] == 16);
    assert(!stats.jobs && !stats.bytes);
    read_exact(path, expected, sizeof(expected));
    assert(DeleteFileA(path));

    CloseHandle(store.thread); /* fixture only: worker has joined */
    CloseHandle(write_entered);
    CloseHandle(write_release);
    CloseHandle(allocate_entered);
    CloseHandle(allocate_release);
    assert(RemoveDirectoryA(directory));
    puts("PASS shader store: owned bytes, bounded queue, failures, stop race, restart");
    return 0;
}
