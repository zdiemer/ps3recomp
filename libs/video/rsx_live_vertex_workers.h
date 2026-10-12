#ifndef RSX_LIVE_VERTEX_WORKERS_H
#define RSX_LIVE_VERTEX_WORKERS_H
#include <xmmintrin.h>
/* Renderer-owned native conversion workers. They read prepared mappings and
 * write disjoint output ranges; they never call D3D or a guest resolver. Every
 * job joins before the renderer changes its plan, references, or scratch data. */
typedef struct {
    HANDLE thread, job, done;
    volatile LONG stop;
    const rsx_vertex_fetch_plan* plan;
    const rsx_vertex_ref* refs;
    u8* out;
    u32 count, mxcsr;
    unsigned modes;
    int result;
} ld_vertex_worker;
static ld_vertex_worker g_vertex_workers[3];
static u32 g_vertex_workers_created;
static u8* g_vertex_worker_audit_buffer;
static size_t g_vertex_worker_audit_capacity;

static DWORD WINAPI ld_vertex_worker_main(void* user)
{
    ld_vertex_worker* worker=(ld_vertex_worker*)user;
    for (;;) {
        if (WaitForSingleObject(worker->job,INFINITE)!=WAIT_OBJECT_0) return 1;
        if (InterlockedCompareExchange(&worker->stop,0,0)) return 0;
        _mm_setcsr(worker->mxcsr);
        worker->result=rsx_vertex_fetch_many_mode(worker->plan,worker->refs,
            worker->count,worker->out,worker->modes);
        SetEvent(worker->done);
    }
}

static void ld_vertex_workers_clear(void)
{
    for (u32 i=0;i<g_vertex_workers_created;++i) {
        ld_vertex_worker* worker=&g_vertex_workers[i];
        InterlockedExchange(&worker->stop,1);
        SetEvent(worker->job);
        WaitForSingleObject(worker->thread,INFINITE);
        CloseHandle(worker->thread);
        CloseHandle(worker->job);
        CloseHandle(worker->done);
    }
    memset(g_vertex_workers,0,sizeof(g_vertex_workers));
    g_vertex_workers_created=0;
    free(g_vertex_worker_audit_buffer);
    g_vertex_worker_audit_buffer=NULL;
    g_vertex_worker_audit_capacity=0;
}

static int ld_vertex_workers_reserve(u32 count)
{
    while (g_vertex_workers_created<count) {
        ld_vertex_worker* worker=&g_vertex_workers[g_vertex_workers_created];
        worker->job=CreateEventW(NULL,FALSE,FALSE,NULL);
        worker->done=CreateEventW(NULL,FALSE,FALSE,NULL);
        if (worker->job && worker->done)
            worker->thread=CreateThread(NULL,0,ld_vertex_worker_main,worker,0,NULL);
        if (!worker->thread) {
            if (worker->job) CloseHandle(worker->job);
            if (worker->done) CloseHandle(worker->done);
            memset(worker,0,sizeof(*worker));
            g_vertex_worker_failed=1;
            fprintf(stderr,"[vertex-workers] creation failed; retaining serial conversion\n");
            return 0;
        }
        ++g_vertex_workers_created;
    }
    return 1;
}

static int ld_vertex_worker_eligible(const rsx_vertex_fetch_plan* plan,
    const rsx_vertex_ref* refs,u32 count)
{
    if (!plan || !refs || !count || !plan->layout.stride || plan->inline_data) return 0;
    u32 first=UINT32_MAX,last=0;
    for (u32 i=0;i<count;++i) {
        const u32 element=(refs[i].vertex_id+refs[i].base_index)&0xFFFFFu;
        if (element<first) first=element;
        if (element>last) last=element;
    }
    for (u32 slot=0;slot<plan->layout.count;++slot) {
        const rsx_vertex_fetch_attr* attr=&plan->attr[plan->layout.attrs[slot]];
        if (attr->desc.type && attr->desc.size &&
            (!attr->stable_base || attr->desc.frequency>1 ||
             first<attr->stable_first || last>attr->stable_last)) return 0;
    }
    return 1;
}

static int ld_vertex_worker_sources(const rsx_vertex_fetch_plan* plan,
    ld_geom_source sources[16],u32* count)
{
    *count=0;
    for (u32 slot=0;slot<plan->layout.count;++slot) {
        const rsx_vertex_fetch_attr* attr=&plan->attr[plan->layout.attrs[slot]];
        if (!attr->desc.type || !attr->desc.size) continue;
        const u64 bytes=(u64)(attr->stable_last-attr->stable_first)*attr->stride+attr->elem_size;
        const u64 offset=(u64)plan->base_offset+attr->desc.offset+(u64)attr->stable_first*attr->stride;
        if (!bytes || bytes>UINT32_MAX || offset>UINT32_MAX || offset+bytes>0x100000000ull) return 0;
        ld_geom_source* source=&sources[(*count)++];
        source->location=attr->desc.location;
        source->offset=(u32)offset; source->bytes=(u32)bytes;
        source->pointer=attr->stable_base;
    }
    return 1;
}

static int ld_vertex_worker_validate_output(const rsx_vertex_fetch_plan* plan,
    const rsx_vertex_ref* refs,u32 count,u8* out,unsigned modes,int ok,
    const ld_geom_source* sources,u32 n_sources,u64 before)
{
    const int serial_ok=rsx_vertex_fetch_many_mode(plan,refs,count,g_vertex_worker_audit_buffer,modes);
    u64 after=0;
    ++g_vertex_worker_stats.audits;
    if (!ld_geom_stamp(sources,n_sources,&after) || before!=after)
        ++g_vertex_worker_stats.races;
    else if (ok!=serial_ok || (ok && memcmp(out,g_vertex_worker_audit_buffer,(size_t)count*plan->layout.stride))) {
        ++g_vertex_worker_stats.failures;
        g_vertex_worker_failed=1;
        fprintf(stderr,"[vertex-workers] stable-source output mismatch; disabling parallel conversion\n");
    }
    if (serial_ok) memcpy(out,g_vertex_worker_audit_buffer,(size_t)count*plan->layout.stride);
    return serial_ok;
}

static int ld_vertex_fetch_parallel(const rsx_vertex_fetch_plan* plan,
    const rsx_vertex_ref* refs,u32 count,u8* out,unsigned modes)
{
    ++g_vertex_worker_stats.draws;
    g_vertex_worker_stats.vertices+=count;
    if (count>=4096) {
        ++g_vertex_worker_stats.large_draws;
        g_vertex_worker_stats.large_vertices+=count;
    }
    const u32 workers=(u32)ld_vertex_worker_count();
    if (!workers || g_vertex_worker_failed || count<ld_vertex_worker_minimum() ||
        !ld_vertex_worker_eligible(plan,refs,count) || !ld_vertex_workers_reserve(workers)) {
        if (workers) ++g_vertex_worker_stats.bypasses;
        return rsx_vertex_fetch_many_mode(plan,refs,count,out,modes);
    }
    ld_geom_source sources[16];
    u32 n_sources=0;
    u64 before=0;
    const int audit=ld_vertex_worker_audit_enabled();
    if (audit) {
        const u64 bytes=(u64)count*plan->layout.stride;
        if (bytes>SIZE_MAX || !ld_vertex_worker_sources(plan,sources,&n_sources) ||
            !ld_geom_stamp(sources,n_sources,&before))
            return rsx_vertex_fetch_many_mode(plan,refs,count,out,modes);
        if (g_vertex_worker_audit_capacity<(size_t)bytes) {
            u8* buffer=(u8*)realloc(g_vertex_worker_audit_buffer,(size_t)bytes);
            if (!buffer) return rsx_vertex_fetch_many_mode(plan,refs,count,out,modes);
            g_vertex_worker_audit_buffer=buffer;
            g_vertex_worker_audit_capacity=(size_t)bytes;
        }
    }
    u32 chunk=count/(workers+1);
    if (chunk>=64) chunk&=~63u;
    const u32 mxcsr=_mm_getcsr();
    u32 submitted=0;
    for (;submitted<workers;++submitted) {
        ld_vertex_worker* worker=&g_vertex_workers[submitted];
        worker->plan=plan; worker->refs=refs+submitted*chunk;
        worker->out=out+(u64)submitted*chunk*plan->layout.stride;
        worker->count=chunk; worker->modes=modes; worker->mxcsr=mxcsr;
        if (!SetEvent(worker->job)) break;
    }
    const u32 first=submitted*chunk;
    int ok=rsx_vertex_fetch_many_mode(plan,refs+first,count-first,
        out+(u64)first*plan->layout.stride,modes);
#if defined(YZ_PERF_PROFILE)
    const LONGLONG wait_begin=ld_profile_qpc();
#endif
    for (u32 i=0;i<submitted;++i) {
        if (WaitForSingleObject(g_vertex_workers[i].done,INFINITE)!=WAIT_OBJECT_0 ||
            !g_vertex_workers[i].result) ok=0;
    }
#if defined(YZ_PERF_PROFILE)
    g_vertex_worker_wait_ticks+=(u64)(ld_profile_qpc()-wait_begin);
#endif
    if (submitted!=workers) {
        g_vertex_worker_failed=1;
        ++g_vertex_worker_stats.failures;
    }
    ++g_vertex_worker_stats.parallel_draws;
    g_vertex_worker_stats.parallel_vertices+=count;
    g_vertex_worker_stats.jobs+=submitted;
    if (audit) ok=ld_vertex_worker_validate_output(plan,refs,count,out,modes,ok,sources,n_sources,before);
    return ok;
}
#endif
