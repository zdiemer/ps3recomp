/* Extracted production terminal shader-cache transfer, real Win32 threads and
 * bundle persistence; only allocation/thread failures and blobs are mocked. */
#define _CRT_SECURE_NO_WARNINGS
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3dcommon.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#error Assertions required.
#endif
static HANDLE lock_attempted;
static volatile LONG observe_lock;
static BOOL WINAPI observed_lock(HANDLE file, DWORD flags, DWORD reserved,
    DWORD low, DWORD high, LPOVERLAPPED state)
{
    BOOL result = LockFileEx(file, flags, reserved, low, high, state);
    if (InterlockedCompareExchange(&observe_lock, 0, 0)) SetEvent(lock_attempted);
    return result;
}
#define LockFileEx observed_lock
#include "rsx_shader_bundle.h"
#undef LockFileEx

/* Actual cache typedefs and size constant from the renderer. */
#include "shader_cache_types_actual.inc"
#include "shader_cache_append_actual.inc"
static struct { int ready; shader_blob_cache_t vs_blobs, ps_blobs; } g;
static int g_ld_shader_disk_ready = 1;
static volatile LONG g_shader_bundle_dump_state, g_shader_bundle_saved_entries;
static volatile LONG64 g_shader_bundle_saved_bytes;
static char bundle_path[MAX_PATH];
static int shader_bundle_path(char out[MAX_PATH])
{
    memcpy(out, bundle_path, strlen(bundle_path) + 1); return 1;
}
static volatile LONG released_blobs, freed_sources;
static void* tracked_sources[8];
static unsigned tracked_count;
static volatile LONG allocation_calls, fail_allocation;
static int fail_thread;
static unsigned thread_calls;
static HANDLE worker_handle;
static void* injected_calloc(size_t count, size_t size)
{
    LONG ordinal = InterlockedIncrement(&allocation_calls);
    if (ordinal == InterlockedCompareExchange(&fail_allocation, 0, 0)) return NULL;
    return calloc(count, size);
}
static void tracked_free(void* pointer)
{
    for (unsigned i = 0; i < tracked_count; ++i)
        if (pointer && pointer == tracked_sources[i]) {
            InterlockedIncrement(&freed_sources); tracked_sources[i] = NULL;
        }
    free(pointer);
}
static HANDLE WINAPI injected_thread(LPSECURITY_ATTRIBUTES attributes,
    SIZE_T stack, LPTHREAD_START_ROUTINE entry, LPVOID argument,
    DWORD flags, LPDWORD id)
{
    ++thread_calls;
    if (fail_thread) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    HANDLE thread = CreateThread(attributes, stack, entry, argument, flags, id);
    if (thread) assert(DuplicateHandle(GetCurrentProcess(), thread,
        GetCurrentProcess(), &worker_handle, 0, FALSE, DUPLICATE_SAME_ACCESS));
    return thread;
}
#define calloc injected_calloc
#define free tracked_free
#define CreateThread injected_thread
#include "shader_shutdown_actual.inc"
#undef CreateThread
#undef free
#undef calloc
#define free tracked_free
#include "shader_cache_release_actual.inc"
#undef free

typedef struct fake_blob { ID3DBlob iface; LONG refs; unsigned char bytes[8]; } fake_blob;
static HRESULT STDMETHODCALLTYPE blob_query(ID3DBlob* self, REFIID iid, void** out)
{ (void)self; (void)iid; *out = NULL; return E_NOINTERFACE; }
static ULONG STDMETHODCALLTYPE blob_addref(ID3DBlob* self)
{ return (ULONG)InterlockedIncrement(&((fake_blob*)self)->refs); }
static ULONG STDMETHODCALLTYPE blob_release(ID3DBlob* self)
{
    fake_blob* blob = (fake_blob*)self; LONG refs = InterlockedDecrement(&blob->refs);
    assert(refs >= 0);
    if (!refs) { InterlockedIncrement(&released_blobs); free(blob); }
    return (ULONG)refs;
}
static LPVOID STDMETHODCALLTYPE blob_pointer(ID3DBlob* self)
{ return ((fake_blob*)self)->bytes; }
static SIZE_T STDMETHODCALLTYPE blob_size(ID3DBlob* self)
{ (void)self; return 8; }
static ID3D10BlobVtbl blob_vtable = {blob_query,blob_addref,blob_release,blob_pointer,blob_size};
static void add_entry(shader_blob_cache_t* cache, const char* source, unsigned char value)
{
    assert(cache->count < MAX_SHADER_BLOBS && tracked_count < 8);
    shader_blob_t* entry = &cache->entries[cache->count++];
    entry->source_length = (u32)strlen(source);
    entry->source = (char*)malloc(entry->source_length + 1); assert(entry->source);
    memcpy(entry->source, source, entry->source_length + 1);
    entry->hash = shader_bundle_hash((const u8*)source, entry->source_length);
    fake_blob* blob = (fake_blob*)calloc(1,sizeof(*blob)); assert(blob);
    blob->iface.lpVtbl = &blob_vtable; blob->refs = 1; memset(blob->bytes,value,8);
    entry->blob = &blob->iface; tracked_sources[tracked_count++] = entry->source;
    cache->retained_source_bytes += entry->source_length;
    cache->retained_blob_bytes += 8;
}
static void join_worker(void)
{
    if (worker_handle) {
        assert(WaitForSingleObject(worker_handle,10000) == WAIT_OBJECT_0);
        CloseHandle(worker_handle); worker_handle = NULL;
    }
}
static void reset(void)
{
    join_worker();
    shader_blob_cache_release(&g.vs_blobs); shader_blob_cache_release(&g.ps_blobs);
    memset(&g,0,sizeof(g)); g.ready = 1;
    g_shader_bundle_dump_state = g_shader_bundle_saved_entries = 0;
    g_shader_bundle_saved_bytes = 0;
    allocation_calls = fail_allocation = 0; fail_thread = 0; thread_calls = 0;
    released_blobs = freed_sources = 0; tracked_count = 0;
    memset(tracked_sources,0,sizeof(tracked_sources));
    observe_lock = 0; ResetEvent(lock_attempted); DeleteFileA(bundle_path);
}
static void seed_archive(void)
{
    const u8 source[] = "prior", blob[4] = {1,2,3,4};
    shader_bundle_item item = {0}; item.stage = 'V'; item.source = source;
    item.source_length = 5; item.hash = shader_bundle_hash(source,5);
    item.blob = blob; item.blob_length = 4;
    assert(shader_bundle_persist_file(bundle_path,&item,1,NULL,NULL));
}
static u8* read_archive(size_t* size, shader_bundle_view* view)
{
    u8* data = NULL; assert(shader_bundle_read_file(bundle_path,&data,size,view) == 1);
    return data;
}
static void check_saved(void)
{
    size_t size; shader_bundle_view view; u8* data = read_archive(&size,&view);
    assert(view.count == 3);
    const char* sources[2] = {"owned-v", "owned-p"};
    const u32 stages[2] = {'V','P'}; const unsigned char values[2] = {0x31,0x42};
    for (unsigned i = 0; i < 2; ++i) {
        const shader_bundle_record* r = shader_bundle_find(&view,stages[i],sources[i],7,
            shader_bundle_hash((const u8*)sources[i],7)); assert(r && r->blob_length == 8);
        for (unsigned j = 0; j < 8; ++j) assert(view.bytes[r->blob_offset+j] == values[i]);
    }
    free(data);
}
static void fill(void)
{ add_entry(&g.vs_blobs,"owned-v",0x31); add_entry(&g.ps_blobs,"owned-p",0x42); }
static void test_success(void)
{
    reset(); seed_archive(); fill();
    assert(rsx_live_draw_finish_shader_bundle(10000)); join_worker();
    assert(!g.vs_blobs.count && !g.ps_blobs.count && released_blobs == 2 && freed_sources == 2);
    assert(g_shader_bundle_dump_state == 2 && g_shader_bundle_saved_entries == 3);
    check_saved(); assert(rsx_live_draw_finish_shader_bundle(0) && thread_calls == 1);
    puts("bundle shutdown success, owned bytes, fresh read and repeated call passed");
}
static void test_timeout(void)
{
    reset(); seed_archive(); fill();
    HANDLE held; OVERLAPPED state; assert(shader_bundle_lock_file(bundle_path,&held,&state));
    size_t old_size; shader_bundle_view old_view; u8* old = read_archive(&old_size,&old_view);
    observe_lock = 1;
    assert(!rsx_live_draw_finish_shader_bundle(1));
    assert(WaitForSingleObject(lock_attempted,2000) == WAIT_OBJECT_0);
    assert(!g.vs_blobs.count && !g.ps_blobs.count && !released_blobs && !freed_sources);
    assert(!rsx_live_draw_finish_shader_bundle(1) && thread_calls == 1);
    size_t now_size; shader_bundle_view now_view; u8* now = read_archive(&now_size,&now_view);
    assert(old_size == now_size && !memcmp(old,now,old_size)); free(old); free(now);
    /* Prove the worker owns detached records and never observes later g. */
    add_entry(&g.vs_blobs,"later-renderer",0x99); g.ready = 0;
    assert(UnlockFileEx(held,0,1,0,&state)); CloseHandle(held); join_worker();
    assert(released_blobs == 2 && freed_sources == 2 && g.vs_blobs.count == 1);
    assert(g.vs_blobs.entries[0].blob->lpVtbl->GetBufferSize(g.vs_blobs.entries[0].blob) == 8);
    check_saved(); puts("bundle shutdown bounded timeout, old archive and late owned completion passed");
}
static void test_failures(void)
{
    for (unsigned failure = 1; failure <= 3; ++failure) {
        reset(); seed_archive(); fill();
        size_t before_size; shader_bundle_view before_view; u8* before = read_archive(&before_size,&before_view);
        if (failure == 1) fail_allocation = 1; /* snapshot metadata allocation */
        else if (failure == 2) fail_thread = 1;
        else fail_allocation = 2; /* worker item table allocation */
        assert(!rsx_live_draw_finish_shader_bundle(10000)); join_worker();
        size_t after_size; shader_bundle_view after_view; u8* after = read_archive(&after_size,&after_view);
        assert(before_size == after_size && !memcmp(before,after,before_size)); free(before); free(after);
        if (failure < 3) {
            assert(g.vs_blobs.count == 1 && g.ps_blobs.count == 1);
            assert(!released_blobs && !freed_sources && g_shader_bundle_dump_state == 0);
            fail_allocation = 0; fail_thread = 0;
            assert(rsx_live_draw_finish_shader_bundle(10000)); join_worker(); check_saved();
        } else {
            assert(!g.vs_blobs.count && !g.ps_blobs.count && released_blobs == 2 && freed_sources == 2);
            assert(g_shader_bundle_dump_state == 3);
        }
    }
    puts("bundle shutdown allocation/thread failures, rollback, retry and archive preservation passed");
}
int main(void)
{
    assert(GetFullPathNameA("shader_cache.bundle",MAX_PATH,bundle_path,NULL));
    lock_attempted = CreateEventA(NULL,TRUE,FALSE,NULL); assert(lock_attempted);
    test_success(); test_timeout(); test_failures(); reset(); CloseHandle(lock_attempted);
    puts("5 actual-source shader bundle shutdown cases passed"); return 0;
}
