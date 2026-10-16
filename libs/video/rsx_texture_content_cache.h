/* Renderer-owner-only immutable 2D upload cache. Included after tex_level_t
 * and create_texture_mipped. Every return owns one COM reference; entries own
 * a separate reference. Guest address identity and SRV remaps remain outside
 * this cache. Hash equality is never sufficient for reuse. */
#ifndef RSX_TEXTURE_CONTENT_CACHE_H
#define RSX_TEXTURE_CONTENT_CACHE_H
#include "rsx_texture_hash.h"
#define LD_TEXTURE_CONTENT_ENTRIES 256u
#define LD_TEXTURE_CONTENT_CPU_LIMIT (128ull * 1024 * 1024)
#define LD_TEXTURE_CONTENT_GPU_LIMIT (128ull * 1024 * 1024)
#define LD_TEXTURE_CONTENT_MIPS 14u
typedef struct {
    ID3D12Resource* resource;
    u8* bytes;
    u64 hash, serial, gpu_bytes;
    u32 size, count;
    DXGI_FORMAT format;
    tex_level_t levels[LD_TEXTURE_CONTENT_MIPS];
} ld_texture_content_entry;
static struct {
    ld_texture_content_entry entries[LD_TEXTURE_CONTENT_ENTRIES];
    u8* scratch;
    u32 scratch_size, count;
    u64 cpu_bytes, gpu_bytes, serial;
    u64 requests, hits, misses, bypasses, evictions, compared_bytes, snapshot_bytes;
} g_ld_texture_content;

static void ld_texture_content_remove(u32 index)
{
    ld_texture_content_entry* e=&g_ld_texture_content.entries[index];
    if (!e->resource) return;
    e->resource->lpVtbl->Release(e->resource);
    free(e->bytes);
    g_ld_texture_content.cpu_bytes-=e->size;
    g_ld_texture_content.gpu_bytes-=e->gpu_bytes;
    --g_ld_texture_content.count;
    memset(e,0,sizeof(*e));
}
static void ld_texture_content_clear(void)
{
    for (u32 i=0;i<LD_TEXTURE_CONTENT_ENTRIES;++i) ld_texture_content_remove(i);
    free(g_ld_texture_content.scratch);
    memset(&g_ld_texture_content,0,sizeof(g_ld_texture_content));
}
static u32 ld_texture_content_oldest(void)
{
    u32 oldest=UINT32_MAX;
    for (u32 i=0;i<LD_TEXTURE_CONTENT_ENTRIES;++i) {
        const ld_texture_content_entry* e=&g_ld_texture_content.entries[i];
        if (e->resource && (oldest==UINT32_MAX ||
            e->serial<g_ld_texture_content.entries[oldest].serial)) oldest=i;
    }
    return oldest;
}
/* The snapshot, not potentially changing guest memory, supplies both the key
 * and the upload. Exact level metadata and bytes also reject hash collisions. */
static ID3D12Resource* ld_texture_content_find(DXGI_FORMAT fmt,
    const tex_level_t* levels,u32 count,const u8* bytes,u32 size,u64 hash)
{
    for (u32 i=0;i<LD_TEXTURE_CONTENT_ENTRIES;++i) {
        ld_texture_content_entry* e=&g_ld_texture_content.entries[i];
        if (!e->resource || e->hash!=hash || e->format!=fmt ||
            e->count!=count || e->size!=size) continue;
        u32 m;
        for (m=0;m<count;++m)
            if (e->levels[m].w!=levels[m].w || e->levels[m].h!=levels[m].h ||
                e->levels[m].row_bytes!=levels[m].row_bytes ||
                e->levels[m].rows!=levels[m].rows) break;
        if (m!=count) continue;
        g_ld_texture_content.compared_bytes+=size;
        if (memcmp(e->bytes,bytes,size)) continue;
        e->serial=++g_ld_texture_content.serial;
        e->resource->lpVtbl->AddRef(e->resource);
        return e->resource;
    }
    return NULL;
}
static ID3D12Resource* create_texture_mipped_cached_scoped(
    DXGI_FORMAT fmt,const tex_level_t* levels,u32 count,
    ld_slow_texture_detail* slow_detail)
{
    ++g_ld_texture_content.requests;
    u64 size=0;
    if (!count || count>LD_TEXTURE_CONTENT_MIPS) goto bypass;
    for (u32 m=0;m<count;++m) {
        if (!levels[m].data || !levels[m].w || !levels[m].h ||
            !levels[m].row_bytes || !levels[m].rows) goto bypass;
        size+=(u64)levels[m].row_bytes*levels[m].rows;
        if (size>LD_TEXTURE_CONTENT_CPU_LIMIT) goto bypass;
    }
    if (size>g_ld_texture_content.scratch_size) {
        while (g_ld_texture_content.cpu_bytes+size>LD_TEXTURE_CONTENT_CPU_LIMIT) {
            const u32 oldest=ld_texture_content_oldest();
            if (oldest==UINT32_MAX) goto bypass;
            ld_texture_content_remove(oldest);
            ++g_ld_texture_content.evictions;
        }
        u8* larger=(u8*)realloc(g_ld_texture_content.scratch,(size_t)size);
        if (!larger) goto bypass;
        g_ld_texture_content.scratch=larger;
        g_ld_texture_content.scratch_size=(u32)size;
    }
    tex_level_t snapshot[LD_TEXTURE_CONTENT_MIPS];
    u32 offset=0;
    for (u32 m=0;m<count;++m) {
        snapshot[m]=levels[m];
        const u32 bytes=levels[m].row_bytes*levels[m].rows;
        memcpy(g_ld_texture_content.scratch+offset,levels[m].data,bytes);
        snapshot[m].data=g_ld_texture_content.scratch+offset;
        offset+=bytes;
    }
    g_ld_texture_content.snapshot_bytes+=size;
    const u64 hash=rsx_texture_hash_xxh3(g_ld_texture_content.scratch,(u32)size);
    ID3D12Resource* resource=ld_texture_content_find(fmt,snapshot,count,
        g_ld_texture_content.scratch,(u32)size,hash);
    if (resource) { ++g_ld_texture_content.hits; return resource; }
    ++g_ld_texture_content.misses;
    resource=create_texture_mipped_scoped(fmt,snapshot,count,slow_detail);
    if (!resource) return NULL;

    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_ALLOCATION_INFO info;
    resource->lpVtbl->GetDesc(resource,&desc);
    g.dev->lpVtbl->GetResourceAllocationInfo(g.dev,&info,0,1,&desc);
    /* Scratch is counted against the CPU budget too. Retaining an oversized
     * resource is optional; the caller still receives its ordinary reference. */
    if (!info.SizeInBytes || info.SizeInBytes>LD_TEXTURE_CONTENT_GPU_LIMIT ||
        size+g_ld_texture_content.scratch_size>LD_TEXTURE_CONTENT_CPU_LIMIT)
        return resource;
    while (g_ld_texture_content.count==LD_TEXTURE_CONTENT_ENTRIES ||
        g_ld_texture_content.cpu_bytes+size+g_ld_texture_content.scratch_size>LD_TEXTURE_CONTENT_CPU_LIMIT ||
        g_ld_texture_content.gpu_bytes+info.SizeInBytes>LD_TEXTURE_CONTENT_GPU_LIMIT) {
        const u32 oldest=ld_texture_content_oldest();
        if (oldest==UINT32_MAX) return resource;
        ld_texture_content_remove(oldest);
        ++g_ld_texture_content.evictions;
    }
    u8* retained=(u8*)malloc((size_t)size);
    if (!retained) return resource;
    memcpy(retained,g_ld_texture_content.scratch,(size_t)size);
    u32 index=0;
    while (g_ld_texture_content.entries[index].resource) ++index;
    ld_texture_content_entry* e=&g_ld_texture_content.entries[index];
    e->resource=resource;
    resource->lpVtbl->AddRef(resource);
    e->bytes=retained; e->size=(u32)size; e->hash=hash;
    e->format=fmt; e->count=count; e->gpu_bytes=info.SizeInBytes;
    e->serial=++g_ld_texture_content.serial;
    offset=0;
    for (u32 m=0;m<count;++m) {
        e->levels[m]=snapshot[m]; e->levels[m].data=retained+offset;
        offset+=snapshot[m].row_bytes*snapshot[m].rows;
    }
    ++g_ld_texture_content.count;
    g_ld_texture_content.cpu_bytes+=size;
    g_ld_texture_content.gpu_bytes+=info.SizeInBytes;
    return resource;
bypass:
    ++g_ld_texture_content.bypasses;
    return create_texture_mipped_scoped(fmt,levels,count,slow_detail);
}
static ID3D12Resource* create_texture_mipped_cached(
    DXGI_FORMAT fmt,const tex_level_t* levels,u32 count)
{
    return create_texture_mipped_cached_scoped(fmt,levels,count,NULL);
}
#endif
