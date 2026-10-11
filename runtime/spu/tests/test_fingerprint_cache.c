#include <assert.h>
#include <stdio.h>
#include "../spu_fingerprint_cache.h"
static DWORD WINAPI worker(void* arg)
{
    uint8_t bytes[4096]; memset(bytes,(int)(uintptr_t)arg,sizeof(bytes));
    for (unsigned i=0; i<400; ++i) {
        bytes[i%sizeof(bytes)]^=0x5A;
        assert(spu_fingerprint_cached(bytes,sizeof(bytes))==spu_fingerprint_bytes(bytes,sizeof(bytes)));
    }
    return 0;
}
int main(void)
{
    uint8_t bytes[4096]; memset(bytes,0x35,sizeof(bytes));
    const uint64_t original=spu_fingerprint_bytes(bytes,sizeof(bytes));
    assert(spu_fingerprint_cached(bytes,sizeof(bytes))==original);
    for (unsigned i=0; i<sizeof(bytes); ++i) {
        bytes[i]^=0x80;
        assert(spu_fingerprint_cached(bytes,sizeof(bytes))==spu_fingerprint_bytes(bytes,sizeof(bytes)));
        bytes[i]^=0x80;
        assert(spu_fingerprint_cached(bytes,sizeof(bytes))==original);
    }
    // Different size at the same EA, including tiny spans and the bounded-cache bypass.
    for (unsigned n=0; n<1024; ++n) assert(spu_fingerprint_cached(bytes,n)==spu_fingerprint_bytes(bytes,n));
    uint8_t* large=calloc(1,1024*1024+1); assert(large);
    assert(spu_fingerprint_cached(large,1024*1024+1)==spu_fingerprint_bytes(large,1024*1024+1)); free(large);
    HANDLE threads[12];
    for (unsigned i=0; i<12; ++i) { threads[i]=CreateThread(NULL,0,worker,(void*)(uintptr_t)(i+1),0,NULL); assert(threads[i]); }
    assert(WaitForMultipleObjects(12,threads,TRUE,10000)==WAIT_OBJECT_0);
    for (unsigned i=0; i<12; ++i) CloseHandle(threads[i]);
    puts("SPU fingerprint cache: exact FNV identity, every-byte mutation, reused EA/size, bounded bypass and concurrent eviction passed");
}
