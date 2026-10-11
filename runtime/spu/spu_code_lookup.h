#ifndef PS3RECOMP_SPU_CODE_LOOKUP_H
#define PS3RECOMP_SPU_CODE_LOOKUP_H

#include "spu_context.h"

typedef void (*spu_code_fn)(spu_context*);
extern spu_code_fn spu_lookup(uint32_t addr, int image_id);

/* Every execution path must resolve the bytes currently resident at the PC.
 * A bounded overlay owns its entire span, including entries not lifted: a
 * miss there must interpret live bytes, never run stale base-image code. */
static inline spu_code_fn spu_lookup_resident(const spu_context* ctx, uint32_t pc)
{
    if (!ctx) return spu_lookup(pc, 0);
    for (unsigned i = 0; i < 4; ++i) {
        if (ctx->resident_code[i].image_id && pc >= ctx->resident_code[i].lsa &&
            pc - ctx->resident_code[i].lsa < ctx->resident_code[i].size)
            return spu_lookup(pc, ctx->resident_code[i].image_id);
    }
    spu_code_fn fn = ctx->resident_task ? spu_lookup(pc, ctx->resident_task) : 0;
    if (!fn && ctx->resident_ovl) fn = spu_lookup(pc, ctx->resident_ovl);
    return fn ? fn : spu_lookup(pc, ctx->image_id);
}

#endif
