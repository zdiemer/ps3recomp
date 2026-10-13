#ifndef RSX_TEXTURE_HASH_H
#define RSX_TEXTURE_HASH_H
#include <stdint.h>
#include <string.h>
#define XXH_INLINE_ALL
#include "../../third_party/xxhash/xxhash.h"

static inline uint64_t rsx_texture_hash_xxh3(const void* data, uint32_t size)
{
    return XXH3_64bits(data, size);
}

/* Full-content mutation check. Four independent word streams remove the
 * serial multiply dependency while still reading every byte, including tails.
 * This hash is process-local cache state, never a disk-cache identity. */
static inline uint64_t rsx_texture_hash(const void* data, uint32_t size)
{
    const unsigned char* p = (const unsigned char*)data;
    uint64_t a=1469598103934665603ull, b=a+1, c=a+2, d=a+3;
    uint32_t i=0;
    for (; size-i >= 32; i+=32) {
        uint64_t w[4]; memcpy(w,p+i,32);
        a=(a^w[0])*1099511628211ull; b=(b^w[1])*1099511628211ull;
        c=(c^w[2])*1099511628211ull; d=(d^w[3])*1099511628211ull;
    }
    a=(a^b)*1099511628211ull;
    a=(a^c)*1099511628211ull;
    a=(a^d)*1099511628211ull;
    for (; size-i >= 8; i+=8) {
        uint64_t w; memcpy(&w,p+i,8); a=(a^w)*1099511628211ull;
    }
    for (; i<size; ++i) a=(a^p[i])*1099511628211ull;
    return (a^size)*1099511628211ull;
}
#endif
