#include "../spu_code_lookup.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void base(spu_context* ctx) { (void)ctx; }
static void overlay(spu_context* ctx) { (void)ctx; }
static void task(spu_context* ctx) { (void)ctx; }
static void second(spu_context* ctx) { (void)ctx; }
spu_code_fn spu_lookup(uint32_t pc, int image)
{
    if (image == 1 && (pc == 0x1000 || pc == 0x1004 || pc == 0x2000 || pc == 0x3000)) return base;
    if (image == 2 && pc == 0x1000) return overlay;
    if (image == 3 && pc == 0x1000) return task;
    if (image == 4 && pc == 0x2000) return second;
    return 0;
}
int main(void)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    assert(ctx);
    ctx->image_id = 1;
    assert(spu_lookup_resident(ctx, 0x1000) == base);
    ctx->resident_ovl = 2;
    assert(spu_lookup_resident(ctx, 0x1000) == overlay);
    ctx->resident_task = 3;
    assert(spu_lookup_resident(ctx, 0x1000) == task);
    ctx->resident_code[0].lsa = 0x1000;
    ctx->resident_code[0].size = 0x100;
    ctx->resident_code[0].image_id = 2;
    ctx->resident_code[1].lsa = 0x2000;
    ctx->resident_code[1].size = 0x100;
    ctx->resident_code[1].image_id = 4;
    assert(spu_lookup_resident(ctx, 0x1000) == overlay);
    assert(spu_lookup_resident(ctx, 0x2000) == second);
    /* The base lift exists here, but its bytes have been overwritten. */
    assert(spu_lookup_resident(ctx, 0x1004) == 0);
    assert(spu_lookup_resident(ctx, 0x3000) == base);
    ctx->resident_code[0].image_id = 0;
    assert(spu_lookup_resident(ctx, 0x1000) == task);
    free(ctx);
    puts("SPU resident code lookup tests passed.");
    return 0;
}
