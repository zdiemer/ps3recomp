#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct rsx_pipeline_recipes { int value; } rsx_pipeline_recipes;
typedef struct rsx_pipeline_recipes_stats {
    unsigned loaded, remembered, prewarmed, prewarm_failed, find_hits, find_misses;
    unsigned unsupported_recipes, capacity_rejections;
    unsigned long long owned_bytes;
} rsx_pipeline_recipes_stats;
static rsx_pipeline_recipes* g_ld_pipeline_recipes;
static HANDLE entered, gate, destroyed;
static volatile LONG writes, releases;
static int save_result=1, fail_thread;
static int rsx_pipeline_recipes_save_atomic(rsx_pipeline_recipes* cache) {
    assert(cache && cache->value==73);
    InterlockedIncrement(&writes);
    SetEvent(entered);
    assert(WaitForSingleObject(gate,5000)==WAIT_OBJECT_0);
    assert(cache->value==73);
    return save_result;
}
static void rsx_pipeline_recipes_destroy(rsx_pipeline_recipes* cache) {
    assert(cache->value==73); free(cache);
    InterlockedIncrement(&releases); SetEvent(destroyed);
}
static void rsx_pipeline_recipes_get_stats(rsx_pipeline_recipes* cache,
    rsx_pipeline_recipes_stats* stats) {
    assert(cache->value==73); stats->remembered=2;
}
static HANDLE test_create_thread(LPSECURITY_ATTRIBUTES a,SIZE_T b,
    LPTHREAD_START_ROUTINE c,LPVOID d,DWORD e,LPDWORD f) {
    return fail_thread ? NULL : CreateThread(a,b,c,d,e,f);
}
#define CreateThread test_create_thread
#include "pipeline_recipe_shutdown_actual.inc"
#undef CreateThread

static void reset_case(void) {
    assert(!g_ld_pipeline_recipe_writer && !g_ld_pipeline_recipes);
    g_ld_pipeline_recipe_save_state=0;
    writes=releases=0; save_result=1; fail_thread=0;
    ResetEvent(entered); ResetEvent(gate); ResetEvent(destroyed);
    g_ld_pipeline_recipes=(rsx_pipeline_recipes*)malloc(sizeof(rsx_pipeline_recipes));
    assert(g_ld_pipeline_recipes); g_ld_pipeline_recipes->value=73;
}
static void await_worker(LONG state) {
    assert(WaitForSingleObject(destroyed,5000)==WAIT_OBJECT_0);
    ULONGLONG deadline=GetTickCount64()+5000;
    while (InterlockedCompareExchange(&g_ld_pipeline_recipe_save_state,0,0)!=state) {
        assert(GetTickCount64()<deadline); Sleep(1);
    }
    assert(writes==1 && releases==1);
}
int main(void) {
    entered=CreateEventW(NULL,TRUE,FALSE,NULL);
    gate=CreateEventW(NULL,TRUE,FALSE,NULL);
    destroyed=CreateEventW(NULL,TRUE,FALSE,NULL);
    assert(entered && gate && destroyed);
    assert(rsx_live_draw_finish_pipeline_recipes(0));

    reset_case();
    assert(!rsx_live_draw_finish_pipeline_recipes(0));
    assert(!g_ld_pipeline_recipes && g_ld_pipeline_recipe_writer);
    assert(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    assert(!rsx_live_draw_finish_pipeline_recipes(1));
    assert(!g_ld_pipeline_recipe_writer && releases==0);
    assert(!rsx_live_draw_finish_pipeline_recipes(0));
    SetEvent(gate); await_worker(2);
    assert(rsx_live_draw_finish_pipeline_recipes(100));
    puts("blocked writer detaches owned cache and survives bounded timeout");

    reset_case(); fail_thread=1;
    rsx_pipeline_recipes* original=g_ld_pipeline_recipes;
    assert(!rsx_live_draw_finish_pipeline_recipes(50));
    assert(g_ld_pipeline_recipes==original && original->value==73 && !writes && !releases);
    fail_thread=0; SetEvent(gate);
    assert(rsx_live_draw_finish_pipeline_recipes(5000)); await_worker(2);
    assert(!g_ld_pipeline_recipes && !g_ld_pipeline_recipe_writer);
    puts("thread creation failure retains cache and retry transfers ownership once");

    reset_case(); save_result=0; SetEvent(gate);
    assert(!rsx_live_draw_finish_pipeline_recipes(5000)); await_worker(3);
    assert(!rsx_live_draw_finish_pipeline_recipes(1));
    puts("failed persistence releases cache and reports failure on repeated calls");

    reset_case(); assert(!rsx_live_draw_finish_pipeline_recipes(0));
    assert(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    SetEvent(gate); await_worker(2);
    assert(rsx_live_draw_finish_pipeline_recipes(5000));
    assert(rsx_live_draw_finish_pipeline_recipes(0));
    assert(!g_ld_pipeline_recipe_writer && writes==1 && releases==1);
    puts("nonblocking start and later join share one writer without duplicate saves");
    CloseHandle(entered); CloseHandle(gate); CloseHandle(destroyed);
    puts("5 actual-source pipeline recipe shutdown cases passed");
    return 0;
}
