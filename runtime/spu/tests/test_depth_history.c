#include <assert.h>
#include <windows.h>
#include "../spu_drain.c"
static volatile LONG halts;
void spu_halt(spu_context* ctx) { (void)ctx; InterlockedIncrement(&halts); }
void spu_check_stack_reset(spu_context* ctx, void (*fn)(spu_context*)) { (void)ctx; (void)fn; assert(0); }
void yz_lockstep_tick(spu_context* ctx) { (void)ctx; assert(0); }
void spu_restart_dispatch(spu_context* ctx) { (void)ctx; assert(0); }
void spu_indirect_branch(spu_context* ctx) { (void)ctx; assert(0); }
void (*spu_lookup(uint32_t pc,int image))(spu_context*) { (void)pc; (void)image; assert(0); return NULL; }
static DWORD WINAPI worker(void* argument)
{
    const uint32_t id=(uint32_t)(uintptr_t)argument;
    spu_context* ctx=calloc(1,sizeof(*ctx)); assert(ctx);
    ctx->host_depth=99;
    for (uint32_t i=0; i<256000; ++i) {
        ctx->pc=id*0x1000+(i & 0xFFF);
        t_recent[t_recent_n++ & 31]=ctx->pc;
        spu_depth_guard(ctx);
    }
    uint32_t recent[24]; assert(spu_recent_pcs(recent,24)==24);
    for (unsigned i=0; i<24; ++i)
        assert(recent[i]==id*0x1000+((256000-24+i) & 0xFFF));
    ctx->host_depth=100; spu_depth_guard(ctx);
    free(ctx);
    return 0;
}
int main(void)
{
    _putenv_s("SPU_HOST_DEPTH_MAX","100");
    _putenv_s("SPU_DEPTH_RING_FILE","");
    HANDLE threads[8];
    for (unsigned i=0; i<8; ++i) {
        threads[i]=CreateThread(NULL,0,worker,(void*)(uintptr_t)(i+1),0,NULL);
        assert(threads[i]);
    }
    assert(WaitForMultipleObjects(8,threads,TRUE,10000)==WAIT_OBJECT_0);
    for (unsigned i=0; i<8; ++i) CloseHandle(threads[i]);
    assert(halts==8);
    puts("SPU depth guard: 8 concurrent histories stay private; every depth limit still halts");
    return 0;
}
