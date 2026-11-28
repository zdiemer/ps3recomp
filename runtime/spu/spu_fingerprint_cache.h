#ifndef SPU_FINGERPRINT_CACHE_H
#define SPU_FINGERPRINT_CACHE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../platform/win32_compat.h"

static uint64_t spu_fingerprint_bytes(const void* data, size_t n)
{
    const uint8_t* p=(const uint8_t*)data;
    uint64_t h=1469598103934665603ULL;
    for (size_t i=0; i<n; ++i) h=(h^p[i])*1099511628211ULL;
    return h;
}

/* Keep the extractor's exact FNV identity. A cache hit requires a FULL byte
 * comparison, so replacing code at a reused EA (including padded DMA tails)
 * cannot reuse the previous image's fingerprint. Bounded shared storage also
 * avoids allocating TLS caches on the short-lived SPURS job threads.
 *
 * Entries match by content, not address: the hash depends only on the bytes.
 * 32 entries hold every image a Haze frame dispatches (~10 interleave); with 8
 * the LRU thrashed, and each miss re-hashed the image byte by byte (~40 us for
 * a 52 KB job image, serialising the 12-job fork-join batch's launches). */
#define SPU_FINGERPRINT_CACHE_ENTRIES 32
static uint64_t spu_fingerprint_cached(const void* data, size_t n)
{
    struct entry { size_t size; uint8_t* bytes; uint64_t hash, age; };
    static struct entry entries[SPU_FINGERPRINT_CACHE_ENTRIES];
    static uint64_t clock;
    static SRWLOCK lock=SRWLOCK_INIT;
    if (n<256 || n>1024*1024) return spu_fingerprint_bytes(data,n);
    AcquireSRWLockExclusive(&lock);
    unsigned oldest=0;
    for (unsigned i=0; i<SPU_FINGERPRINT_CACHE_ENTRIES; ++i) {
        struct entry* e=&entries[i];
        if (e->size==n && !memcmp(e->bytes,data,n)) {
            e->age=++clock;
            const uint64_t hash=e->hash;
            ReleaseSRWLockExclusive(&lock); return hash;
        }
        if (e->age<entries[oldest].age) oldest=i;
    }
    struct entry* e=&entries[oldest];
    uint8_t* bytes=(uint8_t*)realloc(e->bytes,n);
    if (!bytes) {
        ReleaseSRWLockExclusive(&lock); return spu_fingerprint_bytes(data,n);
    }
    e->bytes=bytes; e->size=n; e->age=++clock;
    memcpy(bytes,data,n); e->hash=spu_fingerprint_bytes(bytes,n);
    const uint64_t hash=e->hash;
    ReleaseSRWLockExclusive(&lock); return hash;
}
#endif
