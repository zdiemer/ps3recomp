/* NV0039 memory-to-memory FIFO transfers. Raw GPU bytes are never swapped.
 * Register contract: envytools rnndb/graph/nv_m2mf.xml (MIT).
 * Notification DMA and non-unit byte increments remain unsupported. */
#ifndef RSX_M2MF_H
#define RSX_M2MF_H
#include <stdint.h>
#include <string.h>

typedef struct rsx_m2mf {
    uint32_t dma_in, dma_out, offset_in, offset_out;
    int32_t pitch_in, pitch_out;
    uint32_t length, lines, format;
} rsx_m2mf;
typedef unsigned char* (*rsx_m2mf_resolve)(void*, uint32_t, uint32_t, uint32_t);
typedef void (*rsx_m2mf_move)(void*, unsigned char*, const unsigned char*, uint32_t);

static int rsx_m2mf_copy(const rsx_m2mf* s, rsx_m2mf_resolve resolve,
                        rsx_m2mf_move move, void* opaque)
{
    if (!s->length || !s->lines) return 1;
    if (s->format != 0x101u) return 0;
    /* Validate every row before changing memory; pitches are signed. */
    for (uint64_t y = 0; y < s->lines; ++y) {
        int64_t src = (int64_t)s->offset_in + (int64_t)y * s->pitch_in;
        int64_t dst = (int64_t)s->offset_out + (int64_t)y * s->pitch_out;
        if (src < 0 || dst < 0 || src + s->length > UINT64_C(0x100000000) ||
            dst + s->length > UINT64_C(0x100000000) ||
            !resolve(opaque, s->dma_in, (uint32_t)src, s->length) ||
            !resolve(opaque, s->dma_out, (uint32_t)dst, s->length)) return 0;
    }
    for (uint64_t y = 0; y < s->lines; ++y) {
        uint32_t src = (uint32_t)((int64_t)s->offset_in + (int64_t)y * s->pitch_in);
        uint32_t dst = (uint32_t)((int64_t)s->offset_out + (int64_t)y * s->pitch_out);
        unsigned char* out = resolve(opaque, s->dma_out, dst, s->length);
        const unsigned char* in = resolve(opaque, s->dma_in, src, s->length);
        if (move) move(opaque, out, in, s->length);
        else memmove(out, in, s->length);
    }
    return 1;
}

/* Returns 1 for register updates, 2 for a transfer trigger, 0 otherwise. */
static int rsx_m2mf_method(rsx_m2mf* s, uint32_t method, uint32_t data)
{
    switch (method) {
    case 0x0184: s->dma_in = data; break;
    case 0x0188: s->dma_out = data; break;
    case 0x030C: s->offset_in = data; break;
    case 0x0310: s->offset_out = data; break;
    case 0x0314: s->pitch_in = (int32_t)data; break;
    case 0x0318: s->pitch_out = (int32_t)data; break;
    case 0x031C: s->length = data; break;
    case 0x0320: s->lines = data; break;
    case 0x0324: s->format = data; break;
    case 0x0328: return 2;
    default: return 0;
    }
    return 1;
}
#endif
