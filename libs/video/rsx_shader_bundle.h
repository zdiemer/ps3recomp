#ifndef PS3RECOMP_RSX_SHADER_BUNDLE_H
#define PS3RECOMP_RSX_SHADER_BUNDLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/ps3types.h"
#if defined(_WIN32)
#include <windows.h>
#endif
#ifndef SHADER_DISK_CACHE_VERSION
#define SHADER_DISK_CACHE_VERSION 2u
#endif
#ifndef SHADER_DISK_CACHE_MAX_BLOB
#define SHADER_DISK_CACHE_MAX_BLOB (4u * 1024u * 1024u)
#endif
/* BEGIN RSX SHADER BUNDLE */
#define SHADER_BUNDLE_MAGIC "HZSHBND1"
#define SHADER_BUNDLE_SCHEMA_VERSION 1u
#define SHADER_BUNDLE_COMPILER_TAG 0xD59F4A31u /* D3DCompile vs/ps_5_0, flags=0 */
#define SHADER_BUNDLE_MAX_BYTES (128u * 1024u * 1024u)
#define SHADER_BUNDLE_MAX_ENTRIES 16384u
#define SHADER_BUNDLE_MAX_MERGE_ITEMS (2u * SHADER_BUNDLE_MAX_ENTRIES)
#define SHADER_BUNDLE_MAX_SOURCE (256u * 1024u)
#define SHADER_BUNDLE_MAX_BLOB SHADER_DISK_CACHE_MAX_BLOB

typedef struct shader_bundle_header {
    char magic[8];
    u32 schema_version;
    u32 disk_cache_version;
    u32 compiler_tag;
    u32 entry_count;
    u32 entry_size;
    u32 total_size;
    u32 index_offset;
    u32 data_offset;
} shader_bundle_header;

typedef struct shader_bundle_record {
    u32 stage;
    u32 reserved;
    u64 hash;
    u32 source_length;
    u32 blob_length;
    u32 source_offset;
    u32 blob_offset;
    u64 blob_hash;
} shader_bundle_record;

typedef struct shader_bundle_view {
    const u8* bytes;
    size_t size;
    u32 count;
    const shader_bundle_record* records;
} shader_bundle_view;

typedef struct shader_bundle_item {
    u32 stage;
    u64 hash;
    u32 source_length;
    u32 blob_length;
    const u8* source;
    const u8* blob;
} shader_bundle_item;

typedef char shader_bundle_header_must_be_40_bytes[
    sizeof(shader_bundle_header) == 40 ? 1 : -1];
typedef char shader_bundle_record_must_be_40_bytes[
    sizeof(shader_bundle_record) == 40 ? 1 : -1];

static inline u64 shader_bundle_hash(const u8* source, u32 length)
{
    u64 hash = 1469598103934665603ull;
    u32 i;
    for (i = 0; i < length; ++i) {
        hash ^= source[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static inline u64 shader_bundle_blob_hash(const u8* blob, u32 length)
{
    return shader_bundle_hash(blob, length);
}

static inline int shader_bundle_key_order(
    u32 stage_a, u64 hash_a, u32 length_a, const u8* source_a,
    u32 stage_b, u64 hash_b, u32 length_b, const u8* source_b)
{
    if (stage_a != stage_b) return stage_a < stage_b ? -1 : 1;
    if (hash_a != hash_b) return hash_a < hash_b ? -1 : 1;
    if (length_a != length_b) return length_a < length_b ? -1 : 1;
    if (!length_a) return 0;
    {
        const int result = memcmp(source_a, source_b, length_a);
        return result < 0 ? -1 : result > 0 ? 1 : 0;
    }
}

static inline int shader_bundle_item_compare(const void* left, const void* right)
{
    const shader_bundle_item* a = (const shader_bundle_item*)left;
    const shader_bundle_item* b = (const shader_bundle_item*)right;
    return shader_bundle_key_order(
        a->stage, a->hash, a->source_length, a->source,
        b->stage, b->hash, b->source_length, b->source);
}

/* The v1 layout is canonical: index immediately follows the header; source
 * and blob bytes follow records in index order with no gaps or aliases. */
static inline int shader_bundle_parse(
    const void* data, size_t length, shader_bundle_view* out)
{
    const u8* bytes = (const u8*)data;
    const shader_bundle_header* header;
    const shader_bundle_record* records;
    size_t expected;
    u32 i;
    if (out) memset(out, 0, sizeof(*out));
    if (!bytes || !out || length < sizeof(shader_bundle_header) ||
        length > SHADER_BUNDLE_MAX_BYTES)
        return 0;
    header = (const shader_bundle_header*)bytes;
    if (memcmp(header->magic, SHADER_BUNDLE_MAGIC, 8) != 0 ||
        header->schema_version != SHADER_BUNDLE_SCHEMA_VERSION ||
        header->disk_cache_version != SHADER_DISK_CACHE_VERSION ||
        header->compiler_tag != SHADER_BUNDLE_COMPILER_TAG ||
        header->entry_count > SHADER_BUNDLE_MAX_ENTRIES ||
        header->entry_size != sizeof(shader_bundle_record) ||
        header->total_size != length ||
        header->index_offset != sizeof(shader_bundle_header) ||
        header->data_offset != sizeof(shader_bundle_header) +
            (size_t)header->entry_count * sizeof(shader_bundle_record) ||
        header->data_offset > header->total_size)
        return 0;
    records = (const shader_bundle_record*)(bytes + header->index_offset);
    expected = header->data_offset;
    for (i = 0; i < header->entry_count; ++i) {
        const shader_bundle_record* record = &records[i];
        const u8* source;
        if ((record->stage != (u32)'V' && record->stage != (u32)'P') ||
            record->reserved != 0 ||
            record->source_length > SHADER_BUNDLE_MAX_SOURCE ||
            !record->blob_length ||
            record->blob_length > SHADER_BUNDLE_MAX_BLOB ||
            record->source_offset != expected ||
            record->source_length > header->total_size - expected)
            return 0;
        source = bytes + record->source_offset;
        expected += record->source_length;
        if (record->blob_offset != expected ||
            record->blob_length > header->total_size - expected)
            return 0;
        if (shader_bundle_blob_hash(bytes + record->blob_offset,
                record->blob_length) != record->blob_hash)
            return 0;
        expected += record->blob_length;
        if (shader_bundle_hash(source, record->source_length) != record->hash)
            return 0;
        if (i && shader_bundle_key_order(
                records[i - 1].stage, records[i - 1].hash,
                records[i - 1].source_length,
                bytes + records[i - 1].source_offset,
                record->stage, record->hash, record->source_length,
                source) >= 0)
            return 0;
    }
    if (expected != header->total_size)
        return 0;
    out->bytes = bytes;
    out->size = length;
    out->count = header->entry_count;
    out->records = records;
    return 1;
}

static inline const shader_bundle_record* shader_bundle_find(
    const shader_bundle_view* view, u32 stage, const char* source,
    u32 source_length, u64 hash)
{
    u32 low = 0, high;
    if (!view || !view->bytes || (!source && source_length) ||
        source_length > SHADER_BUNDLE_MAX_SOURCE)
        return NULL;
    high = view->count;
    while (low < high) {
        const u32 middle = low + (high - low) / 2u;
        const shader_bundle_record* record = &view->records[middle];
        if (record->stage < stage ||
            (record->stage == stage && record->hash < hash) ||
            (record->stage == stage && record->hash == hash &&
             record->source_length < source_length))
            low = middle + 1u;
        else
            high = middle;
    }
    while (low < view->count) {
        const shader_bundle_record* record = &view->records[low++];
        if (record->stage != stage || record->hash != hash ||
            record->source_length != source_length)
            break;
        if (!source_length || !memcmp(view->bytes + record->source_offset,
                    source, source_length))
            return record;
    }
    return NULL;
}

static inline int shader_bundle_serialize(
    shader_bundle_item* items, u32 count, u8** out_data, size_t* out_size)
{
    shader_bundle_header* header;
    shader_bundle_record* records;
    size_t total, data_offset;
    u32 i, unique_count;
    u8* bytes;
    if (!out_data || !out_size) return 0;
    *out_data = NULL;
    *out_size = 0;
    if (count > SHADER_BUNDLE_MAX_MERGE_ITEMS || (count && !items)) return 0;
    for (i = 0; i < count; ++i) {
        const shader_bundle_item* item = &items[i];
        if ((item->stage != (u32)'V' && item->stage != (u32)'P') ||
            (!item->source && item->source_length) || !item->blob ||
            item->source_length > SHADER_BUNDLE_MAX_SOURCE ||
            !item->blob_length || item->blob_length > SHADER_BUNDLE_MAX_BLOB ||
            shader_bundle_hash(item->source, item->source_length) != item->hash)
            return 0;
    }
    qsort(items, count, sizeof(*items), shader_bundle_item_compare);
    unique_count = 0;
    for (i = 0; i < count; ++i) {
        if (unique_count && shader_bundle_item_compare(
                &items[unique_count - 1u], &items[i]) == 0) {
            const shader_bundle_item* previous = &items[unique_count - 1u];
            if (previous->blob_length != items[i].blob_length ||
                memcmp(previous->blob, items[i].blob, previous->blob_length))
                return 0;
            continue;
        }
        if (unique_count && items[unique_count - 1u].stage == items[i].stage &&
            items[unique_count - 1u].hash == items[i].hash &&
            items[unique_count - 1u].source_length == items[i].source_length)
            return 0; /* same hash key with distinct bytes: refuse ambiguity */
        if (unique_count != i) items[unique_count] = items[i];
        ++unique_count;
        if (unique_count > SHADER_BUNDLE_MAX_ENTRIES) return 0;
    }
    data_offset = sizeof(shader_bundle_header) +
        (size_t)unique_count * sizeof(shader_bundle_record);
    total = data_offset;
    for (i = 0; i < unique_count; ++i) {
        if (items[i].source_length > SHADER_BUNDLE_MAX_BYTES - total)
            return 0;
        total += items[i].source_length;
        if (items[i].blob_length > SHADER_BUNDLE_MAX_BYTES - total)
            return 0;
        total += items[i].blob_length;
    }
    if (total > SHADER_BUNDLE_MAX_BYTES || total > 0xffffffffu)
        return 0;
    bytes = (u8*)calloc(1, total ? total : sizeof(shader_bundle_header));
    if (!bytes) return 0;
    header = (shader_bundle_header*)bytes;
    memcpy(header->magic, SHADER_BUNDLE_MAGIC, 8);
    header->schema_version = SHADER_BUNDLE_SCHEMA_VERSION;
    header->disk_cache_version = SHADER_DISK_CACHE_VERSION;
    header->compiler_tag = SHADER_BUNDLE_COMPILER_TAG;
    header->entry_count = unique_count;
    header->entry_size = sizeof(shader_bundle_record);
    header->total_size = (u32)total;
    header->index_offset = sizeof(shader_bundle_header);
    header->data_offset = (u32)data_offset;
    records = (shader_bundle_record*)(bytes + header->index_offset);
    total = data_offset;
    for (i = 0; i < unique_count; ++i) {
        shader_bundle_record* record = &records[i];
        const shader_bundle_item* item = &items[i];
        record->stage = item->stage;
        record->hash = item->hash;
        record->source_length = item->source_length;
        record->blob_length = item->blob_length;
        record->source_offset = (u32)total;
        if (item->source_length)
            memcpy(bytes + total, item->source, item->source_length);
        total += item->source_length;
        record->blob_offset = (u32)total;
        memcpy(bytes + total, item->blob, item->blob_length);
        record->blob_hash = shader_bundle_blob_hash(
            bytes + total, item->blob_length);
        total += item->blob_length;
    }
    *out_data = bytes;
    *out_size = total;
    return 1;
}
/* END RSX SHADER BUNDLE */

#if defined(_WIN32)
/* BEGIN RSX SHADER BUNDLE PERSIST */
/* Return 1 for a valid existing bundle, 0 if absent, and -1 for an existing
 * but unreadable/invalid bundle. Callers never overwrite the latter. */
static inline int shader_bundle_read_file(
    const char* path, u8** data, size_t* size, shader_bundle_view* view)
{
    HANDLE file;
    LARGE_INTEGER file_size;
    u8* bytes;
    DWORD got;
    size_t offset = 0;
    if (data) *data = NULL;
    if (size) *size = 0;
    if (view) memset(view, 0, sizeof(*view));
    if (!data || !size || !view) return -1;
    file = CreateFileA(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? 0 : -1;
    }
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0 ||
        (u64)file_size.QuadPart > SHADER_BUNDLE_MAX_BYTES) {
        CloseHandle(file);
        return -1;
    }
    bytes = (u8*)malloc((size_t)file_size.QuadPart);
    if (!bytes) {
        CloseHandle(file);
        return -1;
    }
    while (offset < (size_t)file_size.QuadPart) {
        const DWORD request = (DWORD)(((size_t)file_size.QuadPart - offset) >
            1024u * 1024u ? 1024u * 1024u :
            (size_t)file_size.QuadPart - offset);
        if (!ReadFile(file, bytes + offset, request, &got, NULL) ||
            !got || got > request) {
            free(bytes);
            CloseHandle(file);
            return -1;
        }
        offset += got;
    }
    CloseHandle(file);
    if (!shader_bundle_parse(bytes, offset, view)) {
        free(bytes);
        return -1;
    }
    *data = bytes;
    *size = offset;
    return 1;
}

static inline int shader_bundle_lock_file(
    const char* path, HANDLE* lock_file, OVERLAPPED* lock_state)
{
    char lock_path[MAX_PATH];
    const int length = snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    ULONGLONG start;
    if (length <= 0 || length >= (int)sizeof(lock_path)) return 0;
    *lock_file = CreateFileA(lock_path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (*lock_file == INVALID_HANDLE_VALUE) return 0;
    memset(lock_state, 0, sizeof(*lock_state));
    start = GetTickCount64();
    for (;;) {
        if (LockFileEx(*lock_file,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0, 1, 0, lock_state))
            return 1;
        if (GetLastError() != ERROR_LOCK_VIOLATION ||
            GetTickCount64() - start >= 5000u) {
            CloseHandle(*lock_file);
            *lock_file = INVALID_HANDLE_VALUE;
            return 0;
        }
        Sleep(10);
    }
}

static inline int shader_bundle_persist_file(
    const char* path, const shader_bundle_item* incoming, u32 incoming_count,
    u32* saved_entries, size_t* saved_bytes)
{
    HANDLE lock_file = INVALID_HANDLE_VALUE;
    OVERLAPPED lock_state;
    HANDLE file = INVALID_HANDLE_VALUE;
    char temporary[MAX_PATH] = {0};
    u8* old_bytes = NULL;
    size_t old_size = 0, output_size = 0;
    shader_bundle_view old_view;
    shader_bundle_item* merged = NULL;
    u8* output = NULL;
    u32 count = 0, i;
    int old_result, success = 0;
    LARGE_INTEGER nonce;
    if (saved_entries) *saved_entries = 0;
    if (saved_bytes) *saved_bytes = 0;
    if (!path || incoming_count > SHADER_BUNDLE_MAX_ENTRIES ||
        (incoming_count && !incoming) ||
        !shader_bundle_lock_file(path, &lock_file, &lock_state))
        return 0;

    old_result = shader_bundle_read_file(path, &old_bytes, &old_size, &old_view);
    if (old_result < 0)
        goto done; /* preserve a corrupt/unreadable archive for diagnosis */
    merged = (shader_bundle_item*)calloc(
        SHADER_BUNDLE_MAX_MERGE_ITEMS, sizeof(*merged));
    if (!merged) goto done;
    if (old_result == 1) {
        for (i = 0; i < old_view.count; ++i) {
            const shader_bundle_record* record = &old_view.records[i];
            merged[count].stage = record->stage;
            merged[count].hash = record->hash;
            merged[count].source_length = record->source_length;
            merged[count].blob_length = record->blob_length;
            merged[count].source = old_view.bytes + record->source_offset;
            merged[count].blob = old_view.bytes + record->blob_offset;
            ++count;
        }
    }
    if (incoming_count)
        memcpy(merged + count, incoming,
            (size_t)incoming_count * sizeof(*incoming));
    count += incoming_count;
    if (!shader_bundle_serialize(merged, count, &output, &output_size))
        goto done;

    QueryPerformanceCounter(&nonce);
    {
        const int length = snprintf(temporary, sizeof(temporary),
            "%s.tmp.%lu.%08lX", path, (unsigned long)GetCurrentProcessId(),
            (unsigned long)nonce.LowPart);
        if (length <= 0 || length >= (int)sizeof(temporary)) goto done;
    }
    file = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    {
        size_t offset = 0;
        while (offset < output_size) {
            DWORD wrote = 0;
            const DWORD request = (DWORD)((output_size - offset) >
                1024u * 1024u ? 1024u * 1024u : output_size - offset);
            if (!WriteFile(file, output + offset, request, &wrote, NULL) ||
                !wrote || wrote > request)
                goto done;
            offset += wrote;
        }
    }
    if (!FlushFileBuffers(file)) goto done;
    if (!CloseHandle(file)) goto done;
    file = INVALID_HANDLE_VALUE;
    if (!MoveFileExA(temporary, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        goto done;
    if (saved_entries)
        *saved_entries = ((const shader_bundle_header*)output)->entry_count;
    if (saved_bytes) *saved_bytes = output_size;
    success = 1;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!success && temporary[0]) DeleteFileA(temporary);
    free(output);
    free(merged);
    free(old_bytes);
    if (lock_file != INVALID_HANDLE_VALUE) {
        UnlockFileEx(lock_file, 0, 1, 0, &lock_state);
        CloseHandle(lock_file);
    }
    return success;
}
/* END RSX SHADER BUNDLE PERSIST */
#endif /* _WIN32 */

#endif /* PS3RECOMP_RSX_SHADER_BUNDLE_H */
