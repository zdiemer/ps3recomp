/* Public hook behavior: diagnostic outlining must retain architectural state,
 * exact completion/step rules, history, stack-switch checks and dynamic flow. */
#include <assert.h>
#include <windows.h>
#include "../spu_drain.c"
static unsigned halts, stack_checks;
void spu_halt(spu_context* ctx) { (void)ctx; ++halts; }
void spu_check_stack_reset(spu_context* ctx, void (*fn)(spu_context*))
{ assert(ctx && fn == NULL); ++stack_checks; }
void yz_lockstep_tick(spu_context* ctx) { (void)ctx; assert(0); }
void spu_restart_dispatch(spu_context* ctx) { (void)ctx; assert(0); }
void spu_indirect_branch(spu_context* ctx) { (void)ctx; assert(0); }
void (*spu_lookup(uint32_t pc,int image))(spu_context*)
{ (void)pc; (void)image; assert(0); return NULL; }
static spu_context current, before;
static unsigned expected_checks;
static void check(unsigned expected_steps, unsigned expected_halts)
{
    memcpy(&before, &current, sizeof(current));
    spu_task_launch_check(&current, NULL);
    before.steps = expected_steps;
    assert(current.steps == expected_steps && halts == expected_halts);
    assert(stack_checks == ++expected_checks);
    assert(!memcmp(&current, &before, sizeof(current)));
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    const char* names[] = { "SPU_NO_LS0_END", "SPU_EXITTRACE", "SPU_STEPTRACE",
                           "SPU_STEPTRACE_FROM", "SPU_STEPTRACE_REGS", "SPU_JOBDRAIN" };
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i) _putenv_s(names[i], "");
    const int disabled = !strcmp(argv[1], "no-ls0");
    const int jobdrain = !strcmp(argv[1], "jobdrain");
    if(disabled) _putenv_s("SPU_NO_LS0_END", "1");
    else if(!strcmp(argv[1], "exit")) _putenv_s("SPU_EXITTRACE", "40");
    else if(!strcmp(argv[1], "step")) {
        _putenv_s("SPU_STEPTRACE", "40");
        _putenv_s("SPU_STEPTRACE_FROM", "3400");
        _putenv_s("SPU_STEPTRACE_REGS", "1");
    } else if(jobdrain) _putenv_s("SPU_JOBDRAIN", "1");
    else assert(!strcmp(argv[1], "default"));
    current.ls = current.ls_store; current.image_id = 40;
    for(unsigned i=0;i<32;++i) {
        current.pc = 0x3400 + i*4;
        check(disabled ? 0 : i+1, 0);
    }
    uint32_t history[32]; assert(spu_recent_pcs(history,32)==32);
    for(unsigned i=0;i<32;++i) assert(history[i]==0x3400+i*4);
    /* The flow recorder can arm after the fast path has initialized. */
    g_pm_flow_ctx = &current;
    for(unsigned i=0;i<10;++i) {
        current.pc=0x3500+i*4; check(disabled ? 0 : 33+i, 0);
        assert(g_pm_flow_n == i+1 && g_pm_flow_buf[i] == current.pc);
    }
    g_pm_flow_ctx = NULL;
    current.pc=0x3600; check(disabled ? 0 : 43, 0);
    assert(g_pm_flow_n == 10);
    assert(spu_recent_pcs(history,32)==32);
    for(unsigned i=0;i<21;++i) assert(history[i]==0x3400+(11+i)*4);
    for(unsigned i=0;i<10;++i) assert(history[21+i]==0x3500+i*4);
    assert(history[31]==0x3600); /* No double history write on slow transitions. */
    current.pc=0; current.steps=0;
    check(disabled ? 0 : 1, 0); /* First entry is not completion. */
    check(disabled ? 0 : 2, disabled ? 0 : 1);
    unsigned finished=halts;
    current.policy_mode=1; current.steps=10;
    check(disabled ? 10 : 11, finished);
    current.policy_mode=0; current.image_id=0;
    check(disabled ? 10 : 12, finished);
    current.image_id=40; current.pc=SPU_LS_SIZE; /* Masked PC zero. */
    check(disabled ? 10 : 13, disabled ? finished : finished+1);
    if(jobdrain) {
        current.policy_mode=1; current.image_id=2; current.pc=0x2318;
        current.ls[0xDF3]=1;
        for(unsigned i=0;i<4095;++i) check(current.steps+1, halts);
        assert(current.ls[0xDF0]==0 && current.ls[0xDF3]==1);
        spu_task_launch_check(&current,NULL); ++expected_checks;
        assert(current.ls[0xDF0]==0x80 && current.ls[0xDF3]==1);
        assert(stack_checks==expected_checks); /* Existing real recovery retained. */
    }
    printf("SPU drain hooks (%s): state, history, steps, completion, stack checks and dynamic recorder passed\n",argv[1]);
    return 0;
}
