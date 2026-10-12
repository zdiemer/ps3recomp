#ifndef RSX_TEXTURE_WRITE_WATCH_H
#define RSX_TEXTURE_WRITE_WATCH_H
/* Native Windows dirty-page snapshots. One reader owns all queries/epochs;
 * producers write ordinary memory. Only explicitly registered MEM_WRITE_WATCH
 * reservations are supported. Never reset this reservation outside this owner.
 * No guest addresses, instruction dispatch or producer notification wrappers. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct rsx_texture_write_watch {
    unsigned char* base;
    size_t size, page, capacity;
    uint64_t epoch;
    void** dirty;
    uint64_t* generations;
    int healthy;
} rsx_texture_write_watch;

static inline void rsx_texture_write_watch_destroy(rsx_texture_write_watch* watch)
{
    free(watch->dirty); free(watch->generations);
    memset(watch, 0, sizeof(*watch));
}

/* Call before publishing to the single reader. A failed query disables all
 * future stamps: a partially successful reset must never hide invalidation. */
static inline int rsx_texture_write_watch_init(
    rsx_texture_write_watch* watch, void* base, size_t size)
{
    SYSTEM_INFO info; GetSystemInfo(&info);
    memset(watch, 0, sizeof(*watch));
    if (!base || !size || !info.dwPageSize ||
        (uintptr_t)base % info.dwPageSize || size % info.dwPageSize ||
        size > UINTPTR_MAX - (uintptr_t)base)
        return 0;
    watch->base = (unsigned char*)base;
    watch->size = size; watch->page = info.dwPageSize;
    watch->capacity = size / watch->page;
    if (watch->capacity > SIZE_MAX / sizeof(void*) ||
        watch->capacity > SIZE_MAX / sizeof(uint64_t)) return 0;
    watch->dirty = (void**)malloc(watch->capacity * sizeof(void*));
    watch->generations = (uint64_t*)calloc(watch->capacity, sizeof(uint64_t));
    if (!watch->dirty || !watch->generations) {
        rsx_texture_write_watch_destroy(watch); return 0;
    }
    watch->healthy = 1;
    return 1;
}

/* Capture before reading/hash/decode. A successful unchanged stamp permits
 * reuse of a baseline captured with the same source range and tracker lifetime.
 * Returned dirty pages advance global generations, including pages shared with
 * other textures. Writes after this query stay tracked for the next query. */
static inline int rsx_texture_write_watch_stamp(
    rsx_texture_write_watch* watch, const void* source, size_t length,
    uint64_t* stamp, size_t* dirty_pages)
{
    if (dirty_pages) *dirty_pages = 0;
    if (!watch->healthy || !source || !length || !stamp) return 0;
    const uintptr_t address = (uintptr_t)source, origin = (uintptr_t)watch->base;
    if (address < origin || address-origin >= watch->size ||
        length > watch->size - (address-origin)) return 0;
    const size_t first = (address-origin)/watch->page;
    const size_t last = (address-origin+length-1)/watch->page;
    ULONG_PTR count = last-first+1;
    DWORD granularity = 0;
    if (watch->epoch == UINT64_MAX || GetWriteWatch(WRITE_WATCH_FLAG_RESET,
            watch->base+first*watch->page, count*watch->page,
            watch->dirty, &count, &granularity) ||
        granularity != watch->page || count > last-first+1) {
        watch->healthy = 0; return 0;
    }
    if (count) {
        ++watch->epoch;
        for (size_t i = 0; i < count; ++i) {
            const uintptr_t changed = (uintptr_t)watch->dirty[i];
            if (changed < origin+first*watch->page || changed > origin+last*watch->page ||
                (changed-origin)%watch->page) {
                watch->healthy = 0; return 0;
            }
            watch->generations[(changed-origin)/watch->page] = watch->epoch;
        }
    }
    uint64_t maximum = 0;
    for (size_t i = first; i <= last; ++i)
        if (watch->generations[i] > maximum) maximum = watch->generations[i];
    *stamp = maximum;
    if (dirty_pages) *dirty_pages = count;
    return 1;
}
#endif
