/* Single-producer, bounded diagnostic CSV storage. No file operations occur
 * during recording. Dump only on the owner thread after its measured work.
 * Wrap retains complete recent rows; rejected formatting is counted explicitly.
 * The header must have static lifetime. Abnormal termination loses the capture. */
#ifndef RSX_PROFILE_BUFFER_H
#define RSX_PROFILE_BUFFER_H
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rsx_profile_buffer {
    char* rows;
    size_t capacity, stride;
    uint64_t recorded, rejected;
    const char* header;
    char path[4096];
} rsx_profile_buffer;

static int rsx_profile_buffer_init(rsx_profile_buffer* b, const char* path,
    const char* header, size_t capacity, size_t stride)
{
    if (b->rows || !path || !*path || !header || !capacity || stride < 2 ||
        stride > 8192 || capacity > SIZE_MAX / stride ||
        strlen(path) >= sizeof(b->path)) return 0;
    b->rows = (char*)malloc(capacity * stride);
    if (!b->rows) return 0;
    /* Touch the bounded storage at initialization, before measured windows. */
    memset(b->rows, 0, capacity * stride);
    b->capacity = capacity;
    b->stride = stride;
    b->recorded = b->rejected = 0;
    b->header = header;
    memcpy(b->path, path, strlen(path) + 1);
    return 1;
}

static int rsx_profile_buffer_printf(rsx_profile_buffer* b, const char* format, ...)
{
    char row[8192];
    if (!b->rows) return 0;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(row, b->stride, format, args);
    va_end(args);
    if (n <= 0 || (size_t)n >= b->stride || row[n - 1] != '\n') {
        ++b->rejected;
        return 0;
    }
    memcpy(b->rows + (b->recorded % b->capacity) * b->stride, row, (size_t)n + 1);
    ++b->recorded;
    return 1;
}

static int rsx_profile_buffer_dump(const rsx_profile_buffer* b)
{
    if (!b->rows) return 0;
    FILE* file = fopen(b->path, "wb");
    if (!file) return 0;
    int ok = fputs(b->header, file) >= 0;
    const uint64_t first = b->recorded > b->capacity ? b->recorded - b->capacity : 0;
    for (uint64_t i = first; ok && i < b->recorded; ++i)
        ok = fputs(b->rows + (i % b->capacity) * b->stride, file) >= 0;
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static void rsx_profile_buffer_destroy(rsx_profile_buffer* b)
{
    free(b->rows);
    memset(b, 0, sizeof(*b));
}
#endif
