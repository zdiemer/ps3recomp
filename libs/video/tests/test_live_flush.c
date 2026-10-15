/* Actual ld_flush source is extracted by test_live_flush.py, never rewritten.
 * Standalone via Python from the SDK root (VS developer environment):
 * python -m unittest discover -s libs/video/tests -p test_live_flush.py -v
 * Compile command issued by that driver:
 * clang-cl /nologo /TC /W4 /WX /O2 /I<temp> test_live_flush.c
 *          /Fo<temp>/test_live_flush.obj /Fe<temp>/test_live_flush.exe
 * No D3D device, OS wait, graphics driver, or game is launched by this test.
 */
#define _CRT_SECURE_NO_WARNINGS
#ifdef NDEBUG
#error This fixture requires active assertions
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint32_t DWORD;
typedef int32_t HRESULT;
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED UINT32_MAX
#define INFINITE UINT32_MAX
#define DXGI_ERROR_DEVICE_REMOVED ((HRESULT)0x887A0005u)
#define E_FAIL ((HRESULT)0x80004005u)

typedef enum { LD_FLUSH_PRESENT, LD_FLUSH_DESCRIPTOR_RING, LD_FLUSH_SAMPLER_UPLOAD } ld_flush_reason;
enum {
    LD_SLOW_FLUSH_CLOSE, LD_SLOW_FLUSH_EXECUTE, LD_SLOW_FLUSH_SIGNAL,
    LD_SLOW_FLUSH_COMPLETED_VALUE, LD_SLOW_FLUSH_SET_EVENT, LD_SLOW_FLUSH_WAIT,
    LD_SLOW_FLUSH_INFO_QUEUE, LD_SLOW_FLUSH_RETIRED_RELEASE,
    LD_SLOW_FLUSH_ALLOC_RESET, LD_SLOW_FLUSH_LIST_RESET
};
typedef struct {
    int active;
    struct {
        u64 stage_ticks[10], fence_value, completed_value;
        u32 stage_mask, close_hr, signal_hr, set_event_hr, alloc_reset_hr,
            list_reset_hr, failed_stage, waited, wait_result, wait_error,
            retired_count;
    } event;
} ld_slow_flush_scope;

typedef struct mock_list ID3D12GraphicsCommandList;
typedef struct mock_list ID3D12CommandList;
typedef struct mock_queue ID3D12CommandQueue;
typedef struct mock_fence ID3D12Fence;
typedef struct mock_alloc ID3D12CommandAllocator;
typedef struct mock_resource ID3D12Resource;
typedef struct { unsigned content[16]; } ID3D12DescriptorHeap;
struct list_vtable {
    HRESULT (*Close)(ID3D12GraphicsCommandList*);
    HRESULT (*Reset)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, void*);
};
struct queue_vtable {
    void (*ExecuteCommandLists)(ID3D12CommandQueue*, unsigned, ID3D12CommandList* const*);
    HRESULT (*Signal)(ID3D12CommandQueue*, ID3D12Fence*, u64);
};
struct fence_vtable {
    u64 (*GetCompletedValue)(ID3D12Fence*);
    HRESULT (*SetEventOnCompletion)(ID3D12Fence*, u64, void*);
};
struct alloc_vtable { HRESULT (*Reset)(ID3D12CommandAllocator*); };
struct resource_vtable { unsigned (*Release)(ID3D12Resource*); };
struct mock_list { const struct list_vtable* lpVtbl; };
struct mock_queue { const struct queue_vtable* lpVtbl; };
struct mock_fence { const struct fence_vtable* lpVtbl; };
struct mock_alloc { const struct alloc_vtable* lpVtbl; };
struct mock_resource { const struct resource_vtable* lpVtbl; unsigned releases; };

static struct {
    int ready;
    ID3D12GraphicsCommandList* list;
    ID3D12CommandQueue* queue;
    ID3D12Fence* fence;
    ID3D12CommandAllocator* alloc;
    void* fence_event;
    u64 fence_value;
    ID3D12Resource* retired_textures[2];
    u32 n_retired_textures, upload_used;
    ID3D12DescriptorHeap* smp_heap;
    ID3D12DescriptorHeap* smp_pages[4];
    u32 smp_page, smp_page_count, smp_ring_used;
    int smp_upload_reclaim_pending;
} g;
static struct {
    u64 advances, sampler_exhaustions, srv_exhaustions;
    u64 budget_flushes, upload_flushes, completions;
} g_ld_sampler_pages;
static const u32 g_ld_frames = 17;
static struct {
    HRESULT close_hr, signal_hr, event_hr, alloc_hr, reset_hr;
    u64 completed[2];
    DWORD wait_result;
    int diagnostic;
} scenario;
static struct {
    unsigned close, execute, signal, get_completed, event, wait, release,
             alloc, reset, finish, dred, info, pass_mutation, pass_completed;
    int completion_proved, alloc_succeeded, reset_succeeded;
    u32 failed_stage;
    u64 qpc;
} calls;

static HRESULT mock_close(ID3D12GraphicsCommandList* list) {
    assert(calls.pass_mutation == 1 && calls.pass_completed == 0);
    assert(list == g.list); ++calls.close; return scenario.close_hr;
}
static void mock_execute(ID3D12CommandQueue* queue, unsigned count,
                         ID3D12CommandList* const* lists) {
    assert(queue == g.queue && count == 1 && lists[0] == g.list);
    assert(calls.pass_mutation == 1 && calls.pass_completed == 0);
    assert(calls.close == 1 && !FAILED(scenario.close_hr)); ++calls.execute;
}
static HRESULT mock_signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, u64 value) {
    assert(queue == g.queue && fence == g.fence && value == 42);
    assert(calls.execute == 1); ++calls.signal; return scenario.signal_hr;
}
static u64 mock_completed(ID3D12Fence* fence) {
    assert(fence == g.fence && calls.signal == 1);
    assert(!FAILED(scenario.signal_hr) && calls.get_completed < 2);
    u64 value = scenario.completed[calls.get_completed++];
    if (value != UINT64_MAX && value >= g.fence_value) calls.completion_proved = 1;
    return value;
}
static HRESULT mock_event(ID3D12Fence* fence, u64 value, void* event) {
    assert(fence == g.fence && value == 42 && event == g.fence_event);
    assert(calls.get_completed == 1 && !calls.completion_proved);
    ++calls.event; return scenario.event_hr;
}
static DWORD WaitForSingleObject(void* event, DWORD timeout) {
    assert(event == g.fence_event && timeout == INFINITE);
    assert(calls.event == 1 && !FAILED(scenario.event_hr));
    ++calls.wait; return scenario.wait_result;
}
static DWORD GetLastError(void) { return 123u; }
static unsigned mock_release(ID3D12Resource* resource) {
    assert(calls.completion_proved && "retired resource released before GPU completion");
    assert(resource->releases == 0); ++resource->releases; ++calls.release; return 0;
}
static HRESULT mock_alloc_reset(ID3D12CommandAllocator* allocator) {
    assert(allocator == g.alloc && calls.completion_proved);
    assert(calls.pass_mutation == 1 && calls.pass_completed == 0);
    assert(g.smp_page == 3 && g.smp_ring_used == 7 && g.smp_upload_reclaim_pending);
    assert(g_ld_sampler_pages.completions == 0 && g.upload_used == 12345);
    assert(calls.release == 2); ++calls.alloc;
    calls.alloc_succeeded = !FAILED(scenario.alloc_hr); return scenario.alloc_hr;
}
static HRESULT mock_list_reset(ID3D12GraphicsCommandList* list,
                               ID3D12CommandAllocator* allocator, void* initial_pso) {
    assert(list == g.list && allocator == g.alloc && initial_pso == NULL);
    assert(calls.completion_proved && calls.alloc_succeeded);
    assert(calls.pass_mutation == 1 && calls.pass_completed == 0);
    assert(g.smp_page == 3 && g.smp_ring_used == 7 && g.smp_upload_reclaim_pending);
    assert(g_ld_sampler_pages.completions == 0 && g.upload_used == 12345);
    ++calls.reset; calls.reset_succeeded = !FAILED(scenario.reset_hr); return scenario.reset_hr;
}
static void ld_slow_flush_begin(ld_slow_flush_scope* scope, ld_flush_reason reason) {
    assert(reason == LD_FLUSH_DESCRIPTOR_RING || reason == LD_FLUSH_SAMPLER_UPLOAD);
    memset(scope, 0, sizeof(*scope)); scope->active = scenario.diagnostic;
}
static u64 ld_slow_flush_qpc(void) { return ++calls.qpc; }
static void ld_slow_flush_finish(ld_slow_flush_scope* scope) {
    ++calls.finish; calls.failed_stage = scope->event.failed_stage;
}
static void ld_dump_dred(const char* where, HRESULT hr) {
    assert(where && FAILED(hr)); ++calls.dred;
}
static void ld_drain_info_queue(const char* where) {
    assert(where && calls.completion_proved); ++calls.info;
}

/* This includes the untouched production function, including its real guards
 * and storage-reclamation ordering. Only external interfaces are substituted. */
#include "sampler_pages_actual.inc"
static void ld_native_pass_mutation(const char* reason) {
    assert(reason && strcmp(reason,"flush") == 0);
    assert(g.ready && calls.pass_mutation == 0 && calls.pass_completed == 0);
    /* Invalidate captured state before even closing or submitting its list,
     * including when a later submission/completion/reset step will fail. */
    assert(calls.close == 0 && calls.execute == 0 && calls.signal == 0);
    assert(calls.get_completed == 0 && calls.release == 0 && calls.alloc == 0 && calls.reset == 0);
    ++calls.pass_mutation;
}
static void ld_native_pass_completed(void) {
    /* This hook can destroy quarantined replay resources. A signalled event
     * alone is insufficient: the fence and both reset results must be proven. */
    assert(g.ready && calls.pass_mutation == 1 && calls.pass_completed == 0);
    assert(calls.execute == 1 && calls.signal == 1 && calls.completion_proved);
    assert(calls.alloc == 1 && calls.reset == 1 && calls.alloc_succeeded && calls.reset_succeeded);
    ++calls.pass_completed;
}
#include "live_flush_actual.inc"
#include "sampler_upload_actual.inc"

enum fixture_case {
    EARLY_COMPLETED, DELAYED_COMPLETED, WAIT_FAILURE, UNEXPECTED_WAIT_RESULT,
    INCOMPLETE_AFTER_WAIT, REMOVED_BEFORE_WAIT, REMOVED_AFTER_WAIT,
    CLOSE_FAILURE, SIGNAL_FAILURE, EVENT_FAILURE, ALLOC_FAILURE,
    LIST_FAILURE, EARLY_AHEAD_COMPLETED, FIXTURE_CASE_COUNT
};
static void run_case(enum fixture_case test, int diagnostic, int through_upload_guard) {
    static const struct list_vtable list_methods = {mock_close, mock_list_reset};
    static const struct queue_vtable queue_methods = {mock_execute, mock_signal};
    static const struct fence_vtable fence_methods = {mock_completed, mock_event};
    static const struct alloc_vtable alloc_methods = {mock_alloc_reset};
    static const struct resource_vtable resource_methods = {mock_release};
    ID3D12GraphicsCommandList list = {&list_methods};
    ID3D12CommandQueue queue = {&queue_methods};
    ID3D12Fence fence = {&fence_methods};
    ID3D12CommandAllocator allocator = {&alloc_methods};
    ID3D12Resource resources[2] = {{&resource_methods,0},{&resource_methods,0}};
    ID3D12DescriptorHeap heaps[4];
    memset(&g,0,sizeof(g)); memset(&calls,0,sizeof(calls)); memset(&scenario,0,sizeof(scenario));
    memset(&g_ld_sampler_pages,0,sizeof(g_ld_sampler_pages));
    memset(heaps,0xA5,sizeof(heaps));
    const ID3D12DescriptorHeap before_heaps[4] = {heaps[0],heaps[1],heaps[2],heaps[3]};
    g.ready = 1; g.list = &list; g.queue = &queue; g.fence = &fence; g.alloc = &allocator;
    g.fence_event = &fence; g.fence_value = 41; g.upload_used = 12345;
    g.retired_textures[0] = &resources[0]; g.retired_textures[1] = &resources[1]; g.n_retired_textures = 2;
    for (unsigned i=0;i<4;++i) g.smp_pages[i] = &heaps[i];
    g.smp_page = 3; g.smp_page_count = 4; g.smp_heap = &heaps[3];
    g.smp_ring_used = 7; g.smp_upload_reclaim_pending = 1;
    scenario.completed[0] = 42; scenario.completed[1] = 42; scenario.diagnostic = diagnostic;
    switch (test) {
    case EARLY_COMPLETED: break;
    case EARLY_AHEAD_COMPLETED: scenario.completed[0] = 43; break;
    case DELAYED_COMPLETED: scenario.completed[0] = 41; break;
    case WAIT_FAILURE: scenario.completed[0] = 41; scenario.wait_result = WAIT_FAILED; break;
    case UNEXPECTED_WAIT_RESULT: scenario.completed[0] = 41; scenario.wait_result = WAIT_TIMEOUT; break;
    case INCOMPLETE_AFTER_WAIT: scenario.completed[0] = scenario.completed[1] = 41; break;
    case REMOVED_BEFORE_WAIT: scenario.completed[0] = UINT64_MAX; break;
    case REMOVED_AFTER_WAIT: scenario.completed[0] = 41; scenario.completed[1] = UINT64_MAX; break;
    case CLOSE_FAILURE: scenario.close_hr = E_FAIL; break;
    case SIGNAL_FAILURE: scenario.signal_hr = E_FAIL; break;
    case EVENT_FAILURE: scenario.completed[0] = 41; scenario.event_hr = E_FAIL; break;
    case ALLOC_FAILURE: scenario.alloc_hr = E_FAIL; break;
    case LIST_FAILURE: scenario.reset_hr = E_FAIL; break;
    default: assert(0);
    }
    const int success = test == EARLY_COMPLETED || test == DELAYED_COMPLETED || test == EARLY_AHEAD_COMPLETED;
    if (through_upload_guard) {
        /* This is the actual guard called before texture resource creation or
         * copy recording. A false result must prevent the hypothetical upload. */
        const int can_create_texture = ld_sampler_pages_before_upload();
        assert(can_create_texture == success);
        if (can_create_texture) {
            assert(calls.completion_proved && calls.alloc_succeeded && calls.reset_succeeded);
            assert(g.upload_used == 0 && !g.smp_upload_reclaim_pending);
        }
        assert(g_ld_sampler_pages.upload_flushes == 1);
    } else {
        ld_flush(LD_FLUSH_DESCRIPTOR_RING);
        assert(g_ld_sampler_pages.upload_flushes == 0);
    }
    assert(memcmp(heaps,before_heaps,sizeof(heaps)) == 0);
    assert(calls.finish == 1 && g.ready == success);
    assert(calls.pass_mutation == 1 && calls.pass_completed == (unsigned)success);
    if (success) {
        assert(calls.release == 2 && g.n_retired_textures == 0);
        assert(calls.alloc == 1 && calls.reset == 1 && g_ld_sampler_pages.completions == 1);
        assert(g.upload_used == 0 && g.smp_page == 0 && g.smp_heap == &heaps[0]);
        assert(g.smp_ring_used == 0 && !g.smp_upload_reclaim_pending);
        assert(!calls.failed_stage);
        assert(calls.wait == (test == DELAYED_COMPLETED ? 1u : 0u));
        assert(calls.get_completed == (test == DELAYED_COMPLETED ? 2u : 1u));
    } else {
        assert(g.upload_used == 12345 && g_ld_sampler_pages.completions == 0);
        assert(g.smp_page == 3 && g.smp_heap == &heaps[3]);
        assert(g.smp_ring_used == 7 && g.smp_upload_reclaim_pending == 1);
        if (test == ALLOC_FAILURE || test == LIST_FAILURE) {
            /* Completion was proved; releasing retired resources is safe even
             * though recording cannot restart and storage must stay frozen. */
            assert(calls.release == 2 && g.n_retired_textures == 0);
            assert(calls.alloc == 1 && calls.reset == (test == LIST_FAILURE ? 1u : 0u));
        } else {
            assert(calls.release == 0 && g.n_retired_textures == 2);
            assert(calls.alloc == 0 && calls.reset == 0);
        }
        if (diagnostic) assert(calls.failed_stage != 0);
        /* A failed flush freezes the renderer. Retrying must not invalidate
         * again or call completion on quarantined native-pass resources. */
        unsigned char before_calls[sizeof(calls)], before_state[sizeof(g)];
        memcpy(before_calls,&calls,sizeof(calls)); memcpy(before_state,&g,sizeof(g));
        ld_flush(LD_FLUSH_DESCRIPTOR_RING);
        ld_flush(LD_FLUSH_SAMPLER_UPLOAD);
        assert(!memcmp(before_calls,&calls,sizeof(calls)) && !memcmp(before_state,&g,sizeof(g)));
    }
}
static void test_page_advancement(void) {
    ID3D12DescriptorHeap heaps[4];
    memset(&g,0,sizeof(g)); memset(&calls,0,sizeof(calls));
    memset(&g_ld_sampler_pages,0,sizeof(g_ld_sampler_pages));
    for (unsigned i=0;i<4;++i) {
        memset(&heaps[i],(int)(i+1),sizeof(heaps[i]));
        g.smp_pages[i] = &heaps[i];
    }
    const ID3D12DescriptorHeap before[4] = {heaps[0],heaps[1],heaps[2],heaps[3]};
    g.ready = 1; g.smp_page_count = 4; g.smp_heap = &heaps[0]; g.upload_used = 12345;
    assert(ld_sampler_pages_before_upload() == 1);
    assert(calls.close == 0 && g_ld_sampler_pages.upload_flushes == 0);
    assert(g.upload_used == 12345); /* No pending rollover: no implicit reset. */
    for (unsigned next=1;next<4;++next) {
        g.smp_ring_used = 128;
        assert(ld_sampler_pages_advance() == 1);
        assert(g.smp_page == next && g.smp_heap == &heaps[next]);
        assert(g.smp_ring_used == 0 && g.smp_upload_reclaim_pending == 1);
        assert(g_ld_sampler_pages.advances == next && g_ld_sampler_pages.completions == 0);
        assert(g.upload_used == 12345 && calls.close == 0 && calls.release == 0);
        assert(memcmp(heaps,before,sizeof(heaps)) == 0);
    }
    g.smp_ring_used = 128;
    for (unsigned retry=0;retry<2;++retry) {
        assert(ld_sampler_pages_advance() == 0);
        assert(g.smp_page == 3 && g.smp_heap == &heaps[3] && g.smp_ring_used == 128);
        assert(g.smp_upload_reclaim_pending && g_ld_sampler_pages.advances == 3);
        assert(memcmp(heaps,before,sizeof(heaps)) == 0);
    }
    /* Partial initialization remains bounded by the actual allocated count. */
    g.smp_page = 0; g.smp_heap = &heaps[0]; g.smp_page_count = 1;
    assert(ld_sampler_pages_advance() == 0 && g.smp_heap == &heaps[0]);
    puts("bounded sampler page advancement passed");
}
static void test_terminal_flush_is_noop(void) {
    memset(&g,0,sizeof(g)); memset(&calls,0,sizeof(calls));
    memset(&g_ld_sampler_pages,0,sizeof(g_ld_sampler_pages));
    g.upload_used = 12345; g.smp_page = 3; g.smp_ring_used = 128;
    g.smp_upload_reclaim_pending = 1; g.n_retired_textures = 2;
    /* NULL COM interfaces deliberately fault if the terminal guard is removed. */
    const unsigned char* bytes = (const unsigned char*)&g;
    unsigned char before[sizeof(g)]; memcpy(before,bytes,sizeof(g));
    ld_flush(LD_FLUSH_DESCRIPTOR_RING);
    ld_flush(LD_FLUSH_SAMPLER_UPLOAD);
    assert(memcmp(&g,before,sizeof(g)) == 0);
    assert(calls.close == 0 && calls.finish == 0 && calls.release == 0);
    assert(calls.pass_mutation == 0 && calls.pass_completed == 0);
    assert(g_ld_sampler_pages.completions == 0);
    puts("terminal flush no-op passed");
}
int main(void) {
    test_page_advancement();
    test_terminal_flush_is_noop();
    for (int diagnostic=0; diagnostic<2; ++diagnostic)
        for (int test=0; test<FIXTURE_CASE_COUNT; ++test)
            for (int upload=0;upload<2;++upload)
                run_case((enum fixture_case)test, diagnostic, upload);
    puts("52 actual-source flush/upload cases passed (diagnostic active and inactive)");
    return 0;
}
