#ifndef PS3RECOMP_SPU_MFC_STATE_H
#define PS3RECOMP_SPU_MFC_STATE_H
#include "spu_dma.h"

/* MFC channels are accessed by the executing context's owner thread. Keep
 * their engine on that context rather than in a bounded global slot table. */
static inline mfc_engine* spu_mfc_state_get(spu_context* ctx)
{
    if (!ctx->mfc_state) {
        mfc_engine* engine = (mfc_engine*)malloc(sizeof(*engine));
        if (!engine) {
            fprintf(stderr, "[SPU] cannot allocate private MFC engine\n");
            abort();
        }
        mfc_engine_init(engine);
        ctx->mfc_state = engine;
    }
    return (mfc_engine*)ctx->mfc_state;
}

static inline void spu_mfc_state_release(spu_context* ctx)
{
    if (!ctx) return;
    free(ctx->mfc_state);
    ctx->mfc_state = NULL;
}
#endif
