/* These RI10 instructions already had helpers and lifter support; verify the
 * interpreter's real decoder and execution preserve their vector semantics. */
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}

static const uint8_t input[16] = {
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
    0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF
};

static int run(const char* name, uint8_t op8, uint16_t immediate,
               const uint8_t expected[16]) {
    static spu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    spu_context_init(&ctx, 0);
    ctx.image_id = -1;
    memcpy(ctx.gpr[24]._u8, input, sizeof input);
    memset(ctx.gpr[17]._u8, 0xA5, 16);
    memset(ctx.gpr[16]._u8, 0x5A, 16);
    uint32_t word = ((uint32_t)op8 << 24) | ((uint32_t)(immediate & 0x3FF) << 14)
                  | (24u << 7) | 17u;
    ctx.ls[0x100] = (uint8_t)(word >> 24);
    ctx.ls[0x101] = (uint8_t)(word >> 16);
    ctx.ls[0x102] = (uint8_t)(word >> 8);
    ctx.ls[0x103] = (uint8_t)word;
    ctx.ls[0x104] = ctx.ls[0x105] = ctx.ls[0x106] = ctx.ls[0x107] = 0;
    spu_interp_run(&ctx, 0x100);
    assert(ctx.status == SPU_STATUS_STOPPED_BY_STOP && ctx.pc == 0x108);
    assert(memcmp(ctx.gpr[24]._u8, input, sizeof input) == 0);
    assert(memcmp(ctx.gpr[16]._u8, (uint8_t[16]){
        0x5A,0x5A,0x5A,0x5A,0x5A,0x5A,0x5A,0x5A,
        0x5A,0x5A,0x5A,0x5A,0x5A,0x5A,0x5A,0x5A}, 16) == 0);
    int ok = memcmp(ctx.gpr[17]._u8, expected, 16) == 0;
    printf("%s op8=%02X imm=%03X %s\n", name, op8, immediate, ok ? "PASS" : "FAIL");
    return !ok;
}

int main(void) {
    static const uint8_t orbi[16] = {
        0x12,0x13,0x32,0x33,0x56,0x57,0x76,0x77,
        0x9A,0x9B,0xBA,0xBB,0xDE,0xDF,0xFE,0xFF
    };
    static const uint8_t xorhi[16] = {
        0x55,0x10,0x77,0x32,0x11,0x54,0x33,0x76,
        0xDD,0x98,0xFF,0xBA,0x99,0xDC,0xBB,0xFE
    };
    static const uint8_t xorbi[16] = {
        0xA5,0xB4,0x87,0x96,0xE1,0xF0,0xC3,0xD2,
        0x2D,0x3C,0x0F,0x1E,0x69,0x78,0x4B,0x5A
    };
    int failed = 0;
    failed += run("ORBI", 0x06, 0x012, orbi);
    failed += run("XORHI", 0x45, 0x155, xorhi);
    failed += run("XORBI", 0x46, 0x0A5, xorbi);
    printf("SPU RI10 interpreter: %d failures across 3 opcode checks\n", failed);
    return failed ? 1 : 0;
}
