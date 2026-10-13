/* Grow draw-packet storage without discarding packets after a fixed limit. */
#ifndef RSX_DRAW_BATCHES_H
#define RSX_DRAW_BATCHES_H
#include <stdint.h>
#include <stdlib.h>

typedef struct { uint32_t first, count; } rsx_draw_batch;
static inline int rsx_draw_batch_append(rsx_draw_batch** batches,
                                         uint32_t* used, uint32_t* capacity,
                                         uint32_t first, uint32_t count)
{
    if (*used == *capacity) {
        if (*capacity > UINT32_MAX / 2) return 0;
        uint32_t next = *capacity ? *capacity * 2 : 256;
        if ((uint64_t)next * sizeof(**batches) > SIZE_MAX) return 0;
        rsx_draw_batch* grown = (rsx_draw_batch*)realloc(*batches, (size_t)next * sizeof(*grown));
        if (!grown) return 0;
        *batches = grown;
        *capacity = next;
    }
    (*batches)[*used].first = first;
    (*batches)[*used].count = count;
    ++*used;
    return 1;
}
#endif
