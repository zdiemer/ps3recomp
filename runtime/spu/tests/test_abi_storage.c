/* Verify storage changes preserve fresh state, arguments, local-store writes,
 * mailbox completion, publication lifetime and cleanup order. No game data. */
#include "../spu_lifted_job.h"
#include <assert.h>

uint8_t* vm_base;
static spu_context* published;
static unsigned cleanups, releases, events, runs;
static unsigned scenario;
static uint8_t input[SPU_LS_SIZE], expected[SPU_LS_SIZE];
static const uint32_t argument[4]={0x00400007,0x12345678,0x23456780,0x34567890};
void spu_thread_publish_ctx(uint32_t tid, void* ctx) {
    assert(tid==17); published=(spu_context*)ctx;
}
void spu_coh_unregister(spu_context* ctx) {
    assert(ctx); assert(!published); ++cleanups;
}
void spu_mfc_release(spu_context* ctx) {
    assert(ctx); assert(cleanups==releases+1); ++releases;
}
static void event(uint32_t group,uint32_t tid,int interrupt,uint32_t value) {
    assert(group==9 && tid==17 && interrupt==1 && value==0xCAFE);
    ++events;
}
void (*g_spu_out_mbox_hook)(uint32_t,uint32_t,int,uint32_t)=event;
int spu_run_with_halt(void (*fn)(spu_context*),spu_context* ctx) { fn(ctx); return 0; }
static void job(spu_context* ctx) {
    assert(((uintptr_t)ctx & 15)==0);
    assert(ctx->image_id==40 && ctx->gpr[1]._u32[0]==SPU_LS_SIZE-16);
    for (unsigned r=0;r<128;r++) for (unsigned lane=0;lane<4;lane++) {
        uint32_t value=0;
        if (r==1 && lane==0) value=SPU_LS_SIZE-16;
        if (r==3) value=scenario==0 ? argument[lane] : (lane==0 ? 0x5678 : 0);
        assert(ctx->gpr[r]._u32[lane]==value);
    }
    assert(ctx->host_depth==0 && ctx->steps==0);
    assert(!memcmp(ctx->ls,input,SPU_LS_SIZE));
    for (unsigned i=0;i<SPU_LS_SIZE;i++) ctx->ls[i]^=(uint8_t)(i*13+7);
    ctx->gpr[99]._u32[0]=0xDEADBEEF; /* must not survive the next job */
    if (scenario==1) {
        assert(published==ctx && ctx->spu_id==17 && ctx->spu_group_id==9);
        assert(ctx->park_on_empty_inmbox);
        assert(ctx->ch_in_mbox.value==0xABCD);
        spu_channel_write(&ctx->ch_out_mbox,0xCAFE);
    }
    ++runs;
}
int main(void) {
    const spu_run_opts opts={0xABCD,1,17,9};
    for (unsigned i=0;i<SPU_LS_SIZE;i++) {
        input[i]=(uint8_t)(i*3+1);
        expected[i]=input[i]^(uint8_t)(i*13+7);
    }
    input[0x27C4]=input[0x27C5]=input[0x27C6]=input[0x27C7]=0;
    for (unsigned i=0x27C4;i<0x27C8;i++) expected[i]=input[i]^(uint8_t)(i*13+7);
    uint8_t* ls=(uint8_t*)malloc(SPU_LS_SIZE); assert(ls);
    for (unsigned n=0;n<100;n++) for (unsigned storage=0;storage<3;storage++) {
        scenario=n&1; memcpy(ls,input,SPU_LS_SIZE);
        const spu_run_opts* o=scenario ? &opts : NULL;
        const uint32_t* a=scenario ? NULL : argument;
        int32_t rc;
        if (storage==0) rc=spu_run_lifted_job_abi(job,ls,0x5678,40,!scenario,a,o);
        else if (storage==1) rc=spu_run_lifted_job_abi_heap(job,ls,0x5678,40,!scenario,a,o,NULL);
        else {
            spu_context* ctx=(spu_context*)_aligned_malloc(sizeof(spu_context),16); assert(ctx);
            memset(ctx,0xCD,sizeof(*ctx));
            rc=spu_run_lifted_job_abi_in_context(ctx,job,ls,0x5678,40,!scenario,a,o,NULL);
            _aligned_free(ctx);
        }
        assert(rc==0 && !published);
        assert(!memcmp(ls,expected,SPU_LS_SIZE));
        assert(cleanups==runs && releases==runs);
    }
    assert(runs==300 && events==150); free(ls);
    puts("SPU ABI storage: fresh registers, captured arguments, full LS writes, mailbox completion and cleanup passed");
    return 0;
}
