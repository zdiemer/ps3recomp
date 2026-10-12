#ifndef RSX_LIVE_GEOMETRY_CACHE_H
#define RSX_LIVE_GEOMETRY_CACHE_H
/* Single-renderer native prepared-mesh cache. Included after draw_ctx and its
 * reserve helpers. Index bytes are stamped BEFORE references are gathered;
 * attribute bytes are stamped BEFORE decode. The two generations must stay
 * independent: a later attribute query must not bless older index reads.
 * All page queries share the texture tracker's sole epoch/reset owner. */
enum { LD_GEOM_SLOTS=2048, LD_GEOM_BATCHES=8, LD_GEOM_ATTRS=16 };
#define LD_GEOM_BUDGET (128ull*1024*1024)
typedef struct {
    rsx_vertex_layout_plan layout;
    rsx_dsp_vertex_attr attrs[LD_GEOM_ATTRS];
    float defaults[LD_GEOM_ATTRS][4];
    rsx_dsp_index_array index;
    batch_t arr[LD_GEOM_BATCHES], idx[LD_GEOM_BATCHES];
    u32 n_arr, n_idx, base_offset, base_index, divider_mask;
    u32 primitive, restart_enabled, restart_value, remap_mode;
    void* resolver_user;
} ld_geom_key;
typedef struct { u32 location, offset, bytes; const u8* pointer; } ld_geom_source;
typedef struct {
    ld_geom_key key;
    u64 hash, index_stamp, attr_stamp, bytes;
    ld_geom_source index_sources[LD_GEOM_BATCHES], attr_sources[LD_GEOM_ATTRS];
    u32 n_index_sources, n_attr_sources;
    u32 n_refs, n_verts, n_source_refs, n_cuts, refs_remapped;
    rsx_vertex_fetch_plan plan;
    u8* storage;
} ld_geom_entry;
static ld_geom_entry* g_geom_entries;
static struct {
    ld_geom_entry* entry;
    ld_geom_key key;
    u64 hash, index_stamp, attr_stamp;
    ld_geom_source indices[LD_GEOM_BATCHES], attrs[LD_GEOM_ATTRS];
    u32 n_indices, n_attrs;
    int active, prepared, audit;
} g_geom_draw;

static void ld_geometry_cache_clear(void)
{
    if (g_geom_entries) {
        for (u32 i=0;i<LD_GEOM_SLOTS;++i) free(g_geom_entries[i].storage);
        free(g_geom_entries);
    }
    g_geom_entries=NULL; memset(&g_geom_draw,0,sizeof(g_geom_draw));
    g_geom_stats.bytes=0;
}

static int ld_geom_stamp(const ld_geom_source* sources, u32 count, u64* stamp)
{
    /* Resolve every range again, then merge overlapping/adjacent host pages.
     * Interleaved attributes generally become one native query, not one per
     * attribute. A changed location mapping cannot reuse a former baseline. */
    struct { uintptr_t first,last; } ranges[LD_GEOM_ATTRS];
    u32 n=0;
    *stamp=0;
    if (!g_texture_watch.healthy) return 0;
    for (u32 i=0;i<count;++i) {
        const ld_geom_source* s=&sources[i];
        const u8* pointer=guest_ptr(s->location,s->offset,s->bytes);
        const uintptr_t address=(uintptr_t)pointer, origin=(uintptr_t)g_texture_watch.base;
        if (!pointer || pointer!=s->pointer || !s->bytes || address<origin ||
            address-origin>=g_texture_watch.size || s->bytes>g_texture_watch.size-(address-origin)) return 0;
        const uintptr_t first=address-address%g_texture_watch.page;
        const uintptr_t last=(address+s->bytes-1)-(address+s->bytes-1)%g_texture_watch.page;
        u32 at=n++;
        while (at && ranges[at-1].first>first) { ranges[at]=ranges[at-1]; --at; }
        ranges[at].first=first; ranges[at].last=last;
    }
    for (u32 i=0;i<n;) {
        uintptr_t first=ranges[i].first, last=ranges[i].last;
        while (++i<n && ranges[i].first<=last+g_texture_watch.page)
            if (ranges[i].last>last) last=ranges[i].last;
        u64 value; size_t dirty=0;
#if defined(YZ_PERF_PROFILE)
        const LONGLONG begin=ld_profile_qpc();
#endif
        ++g_geom_stats.queries;
        const int ok=rsx_texture_write_watch_stamp(&g_texture_watch,(const void*)first,
            last-first+g_texture_watch.page,&value,&dirty);
#if defined(YZ_PERF_PROFILE)
        g_geom_query_ticks+=(u64)(ld_profile_qpc()-begin);
#endif
        g_geom_stats.dirty_pages+=dirty;
        if (!ok) return 0;
        if (value>*stamp) *stamp=value;
    }
    return 1;
}

static size_t ld_geom_refs_bytes(const ld_geom_entry* e) { return (size_t)e->n_refs*sizeof(rsx_vertex_ref); }
static size_t ld_geom_map_bytes(const ld_geom_entry* e) { return e->refs_remapped ? (size_t)e->n_source_refs*sizeof(u32) : 0; }
static size_t ld_geom_cuts_bytes(const ld_geom_entry* e) { return (size_t)e->n_cuts*sizeof(u32); }
static size_t ld_geom_payload_bytes(const ld_geom_entry* e) { return (size_t)e->n_verts*e->key.layout.stride; }
static const u8* ld_geom_payload(const ld_geom_entry* e)
{ return e->storage+ld_geom_refs_bytes(e)+ld_geom_map_bytes(e)+ld_geom_cuts_bytes(e); }

static int ld_geom_restore(const ld_geom_entry* e)
{
    if (!reserve_compact_refs(e->n_refs) || !reserve_compact_vertices(ld_geom_payload_bytes(e))) return 0;
    if (e->refs_remapped && dc.ref_remap.occurrence_capacity<e->n_source_refs) {
        u32* map=(u32*)realloc(dc.ref_remap.occurrence_to_unique,ld_geom_map_bytes(e));
        if (!map) return 0;
        dc.ref_remap.occurrence_to_unique=map; dc.ref_remap.occurrence_capacity=e->n_source_refs;
    }
    if (dc.cap_cuts<e->n_cuts) {
        u32* cuts=(u32*)realloc(dc.cuts,ld_geom_cuts_bytes(e));
        if (!cuts) return 0;
        dc.cuts=cuts; dc.cap_cuts=e->n_cuts;
    }
    const u8* source=e->storage;
    if (e->n_refs) memcpy(dc.refs,source,ld_geom_refs_bytes(e)); source+=ld_geom_refs_bytes(e);
    if (e->refs_remapped) memcpy(dc.ref_remap.occurrence_to_unique,source,ld_geom_map_bytes(e)); source+=ld_geom_map_bytes(e);
    if (e->n_cuts) memcpy(dc.cuts,source,ld_geom_cuts_bytes(e));
    memcpy(dc.compact_verts,ld_geom_payload(e),ld_geom_payload_bytes(e));
    dc.n_refs=e->n_refs; dc.n_verts=e->n_verts; dc.n_source_refs=e->n_source_refs;
    dc.n_cuts=e->n_cuts; dc.refs_remapped=(int)e->refs_remapped;
    dc.layout=e->key.layout; dc.fetch_plan=e->plan;
    return 1;
}

static int ld_geom_begin_impl(const rsx_vertex_layout_plan* layout, int packed)
{
    g_geom_draw.active=g_geom_draw.prepared=g_geom_draw.audit=0;
    if (!ld_geometry_cache_enabled()) return 0;
    memset(&g_geom_draw,0,sizeof(g_geom_draw));
    if (!packed || !layout->stride || dc.inl || dc.inl_bytes || !dc.fetch_ok ||
        dc.n_refs || dc.n_arr>LD_GEOM_BATCHES || dc.n_idx>LD_GEOM_BATCHES ||
        !g_texture_watch.healthy || g_geom_cache_failed) goto bypass;
    u64 occurrences=0;
    for (u32 i=0;i<dc.n_arr;++i) occurrences+=dc.arr[i].count;
    for (u32 i=0;i<dc.n_idx;++i) occurrences+=dc.idx[i].count;
    if (occurrences<16 || occurrences>1024*1024) goto bypass;
    ld_geom_key* key=&g_geom_draw.key;
    key->layout=*layout; key->n_arr=dc.n_arr; key->n_idx=dc.n_idx;
    key->primitive=g.rsx.current_primitive; key->resolver_user=g.guest_user;
    key->remap_mode=g_ld_cpu_work&(2u|256u|2048u);
    if (dc.n_arr) memcpy(key->arr,dc.arr,(size_t)dc.n_arr*sizeof(batch_t));
    if (dc.n_idx) memcpy(key->idx,dc.idx,(size_t)dc.n_idx*sizeof(batch_t));
    rsx_vertex_fetch_plan plan;
    rsx_vertex_fetch_plan_init(&plan,&g.rsx,layout,(rsx_vertex_guest_ptr_fn)g.guest_ptr,g.guest_user);
    key->base_offset=plan.base_offset; key->divider_mask=plan.divider_mask;
    for (u32 slot=0;slot<layout->count;++slot) {
        const u32 a=layout->attrs[slot];
        key->attrs[a]=plan.attr[a].desc;
        memcpy(key->defaults[a],plan.attr[a].default_value,sizeof(key->defaults[a]));
    }
    if (dc.n_idx) {
        key->base_index=rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_get_index_array(&g.rsx,&key->index);
        static int no_restart=-1;
        if (no_restart<0) no_restart=getenv("RSX_NO_RESTART")!=NULL;
        key->restart_enabled=!no_restart && rsx_dsp_restart_index_enabled(&g.rsx,key->index.is_u32);
        key->restart_value=rsx_dsp_restart_index(&g.rsx);
        const u32 element=key->index.is_u32?4u:2u;
        for (u32 i=0;i<dc.n_idx;++i) {
            const u64 offset=(u64)key->index.offset+(u64)dc.idx[i].first*element;
            const u64 bytes=(u64)dc.idx[i].count*element;
            if (!bytes) continue;
            if (offset>UINT32_MAX || bytes>UINT32_MAX || offset+bytes>0x100000000ull) goto bypass;
            ld_geom_source* s=&g_geom_draw.indices[g_geom_draw.n_indices++];
            s->location=key->index.location; s->offset=(u32)offset; s->bytes=(u32)bytes;
            s->pointer=guest_ptr(s->location,s->offset,s->bytes);
        }
    }
    if (!g_geom_entries) g_geom_entries=(ld_geom_entry*)calloc(LD_GEOM_SLOTS,sizeof(ld_geom_entry));
    if (!g_geom_entries) goto bypass;
    g_geom_draw.hash=rsx_texture_hash(key,sizeof(*key));
    ld_geom_entry* entry=NULL;
    for (u32 probe=0;probe<4;++probe) {
        ld_geom_entry* e=&g_geom_entries[((u32)g_geom_draw.hash+probe)&(LD_GEOM_SLOTS-1)];
        if (e->storage && e->hash==g_geom_draw.hash && !memcmp(&e->key,key,sizeof(*key))) { entry=e; break; }
        if (!entry || !e->storage) entry=e;
    }
    g_geom_draw.entry=entry;
    /* Must precede all index-byte reads, including misses and audit reads. */
    if (!ld_geom_stamp(g_geom_draw.indices,g_geom_draw.n_indices,&g_geom_draw.index_stamp)) goto bypass;
    g_geom_draw.active=1;
    if (entry->storage && entry->hash==g_geom_draw.hash && !memcmp(&entry->key,key,sizeof(*key)) &&
        entry->index_stamp==g_geom_draw.index_stamp &&
        !memcmp(entry->index_sources,g_geom_draw.indices,sizeof(entry->index_sources)) &&
        ld_geom_stamp(entry->attr_sources,entry->n_attr_sources,&g_geom_draw.attr_stamp) &&
        entry->attr_stamp==g_geom_draw.attr_stamp) {
        if (ld_geometry_cache_validate()) { g_geom_draw.audit=1; ++g_geom_stats.audits; return 0; }
#if defined(YZ_PERF_PROFILE)
        const LONGLONG restore_begin=ld_profile_qpc();
#endif
        const int restored=ld_geom_restore(entry);
#if defined(YZ_PERF_PROFILE)
        g_geom_copy_ticks+=(u64)(ld_profile_qpc()-restore_begin);
#endif
        if (restored) {
            ++g_geom_stats.hits; g_geom_stats.hit_vertices+=entry->n_verts;
            g_geom_stats.hit_refs+=entry->n_source_refs; return 1;
        }
        /* Allocation failure falls through to the ordinary renderer path. */
    }
    ++g_geom_stats.misses;
    return 0;
bypass:
    g_geom_draw.active=0; ++g_geom_stats.bypasses;
    return 0;
}
static int ld_geom_begin(const rsx_vertex_layout_plan* layout, int packed)
{
#if defined(YZ_PERF_PROFILE)
    const LONGLONG begin=ld_profile_qpc();
#endif
    const int hit=ld_geom_begin_impl(layout,packed);
#if defined(YZ_PERF_PROFILE)
    g_geom_begin_ticks+=(u64)(ld_profile_qpc()-begin);
#endif
    return hit;
}

static void ld_geom_prepare(void)
{
    if (!g_geom_draw.active || !dc.fetch_ok || !dc.n_refs) return;
    for (u32 slot=0;slot<dc.layout.count;++slot) {
        const u32 a=dc.layout.attrs[slot];
        const rsx_vertex_fetch_attr* f=&dc.fetch_plan.attr[a];
        if (!f->desc.type || !f->desc.size) continue;
        if (f->desc.type>7 || !f->elem_size || !f->stable_base || f->stable_last<f->stable_first) goto bypass;
        const u64 bytes=(u64)(f->stable_last-f->stable_first)*f->stride+f->elem_size;
        const u64 offset=(u64)dc.fetch_plan.base_offset+f->desc.offset+(u64)f->stable_first*f->stride;
        if (!bytes || bytes>4*1024*1024 || offset>UINT32_MAX || offset+bytes>0x100000000ull) goto bypass;
        ld_geom_source* s=&g_geom_draw.attrs[g_geom_draw.n_attrs++];
        s->location=f->desc.location; s->offset=(u32)offset; s->bytes=(u32)bytes; s->pointer=f->stable_base;
    }
    if (!ld_geom_stamp(g_geom_draw.attrs,g_geom_draw.n_attrs,&g_geom_draw.attr_stamp)) goto bypass;
    g_geom_draw.prepared=1;
    return;
bypass:
    g_geom_draw.active=0; ++g_geom_stats.bypasses;
}

static int ld_geom_equal(const ld_geom_entry* e)
{
    if (e->n_refs!=dc.n_refs || e->n_verts!=dc.n_verts || e->n_source_refs!=dc.n_source_refs ||
        e->n_cuts!=dc.n_cuts || e->refs_remapped!=(u32)dc.refs_remapped ||
        memcmp(&e->key.layout,&dc.layout,sizeof(dc.layout))) return 0;
    const u8* source=e->storage;
    if (e->n_refs && memcmp(source,dc.refs,ld_geom_refs_bytes(e))) return 0; source+=ld_geom_refs_bytes(e);
    if (e->refs_remapped && memcmp(source,dc.ref_remap.occurrence_to_unique,ld_geom_map_bytes(e))) return 0; source+=ld_geom_map_bytes(e);
    if (e->n_cuts && memcmp(source,dc.cuts,ld_geom_cuts_bytes(e))) return 0;
    return !memcmp(ld_geom_payload(e),dc.compact_verts,ld_geom_payload_bytes(e));
}

static void ld_geom_finish_impl(void)
{
    if (!g_geom_draw.active || !g_geom_draw.prepared || !dc.fetch_ok || !dc.n_verts) return;
    ld_geom_entry* e=g_geom_draw.entry;
    if (g_geom_draw.audit) {
        u64 index_stamp,attr_stamp;
        if (!ld_geom_stamp(g_geom_draw.indices,g_geom_draw.n_indices,&index_stamp) ||
            !ld_geom_stamp(g_geom_draw.attrs,g_geom_draw.n_attrs,&attr_stamp) ||
            index_stamp!=e->index_stamp || attr_stamp!=e->attr_stamp) ++g_geom_stats.races;
        else if (!ld_geom_equal(e)) {
            ++g_geom_stats.failures; g_geom_cache_failed=1;
            fprintf(stderr,"[geometry-cache] stable-generation output mismatch; disabling reuse\n");
            return;
        } else {
            ++g_geom_stats.hits; g_geom_stats.hit_vertices+=e->n_verts;
            g_geom_stats.hit_refs+=e->n_source_refs;
        }
    }
    const u64 refs=(u64)dc.n_refs*sizeof(rsx_vertex_ref);
    const u64 map=dc.refs_remapped?(u64)dc.n_source_refs*sizeof(u32):0;
    const u64 cuts=(u64)dc.n_cuts*sizeof(u32);
    const u64 payload=(u64)dc.n_verts*dc.layout.stride;
    const u64 bytes=refs+map+cuts+payload;
    if (!payload || bytes>8*1024*1024 || g_geom_stats.bytes-e->bytes+bytes>LD_GEOM_BUDGET) return;
    u8* storage=(u8*)malloc((size_t)bytes);
    if (!storage) return;
    u8* out=storage;
    if (refs) memcpy(out,dc.refs,(size_t)refs); out+=refs;
    if (map) memcpy(out,dc.ref_remap.occurrence_to_unique,(size_t)map); out+=map;
    if (cuts) memcpy(out,dc.cuts,(size_t)cuts); out+=cuts;
    memcpy(out,dc.compact_verts,(size_t)payload);
    g_geom_stats.bytes=g_geom_stats.bytes-e->bytes+bytes;
    free(e->storage); memset(e,0,sizeof(*e));
    e->storage=storage; e->bytes=bytes; e->hash=g_geom_draw.hash; e->key=g_geom_draw.key;
    e->index_stamp=g_geom_draw.index_stamp; e->attr_stamp=g_geom_draw.attr_stamp;
    e->n_index_sources=g_geom_draw.n_indices; e->n_attr_sources=g_geom_draw.n_attrs;
    memcpy(e->index_sources,g_geom_draw.indices,sizeof(e->index_sources));
    memcpy(e->attr_sources,g_geom_draw.attrs,sizeof(e->attr_sources));
    e->n_refs=dc.n_refs; e->n_verts=dc.n_verts; e->n_source_refs=dc.n_source_refs;
    e->n_cuts=dc.n_cuts; e->refs_remapped=(u32)dc.refs_remapped; e->plan=dc.fetch_plan;
}
static void ld_geom_finish(void)
{
#if defined(YZ_PERF_PROFILE)
    const LONGLONG begin=ld_profile_qpc();
#endif
    ld_geom_finish_impl();
#if defined(YZ_PERF_PROFILE)
    g_geom_store_ticks+=(u64)(ld_profile_qpc()-begin);
#endif
}
#endif
