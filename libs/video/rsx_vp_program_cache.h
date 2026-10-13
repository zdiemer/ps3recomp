#ifndef RSX_VP_PROGRAM_CACHE_H
#define RSX_VP_PROGRAM_CACHE_H

#include "rsx_vp_decompiler.h"

/* No guest addresses or mutable render state are cached. Every hit compares
 * the complete instruction bytes through END, including the END bit itself.
 * Fixed storage bounds memory use and avoids allocation in the draw path. */
#define RSX_VP_CACHE_MAX_BYTES (544u * 16u)
#define RSX_VP_CACHE_SETS 64u
#define RSX_VP_CACHE_WAYS 4u

typedef struct rsx_vp_program_info {
    u32 instructions;
    u32 input_mask;
    u32 txl_mask;
    u64 hash;
} rsx_vp_program_info;

typedef struct rsx_vp_program_entry {
    rsx_vp_program_info info;
    u8 bytes[RSX_VP_CACHE_MAX_BYTES];
} rsx_vp_program_entry;

typedef struct rsx_vp_program_cache {
    rsx_vp_program_entry entries[RSX_VP_CACHE_SETS][RSX_VP_CACHE_WAYS];
    u32 next[RSX_VP_CACHE_SETS];
} rsx_vp_program_cache;

/* cache must initially be zeroed. Returns 1 on a verified content hit, 0 on a
 * miss. Missing END/short/null inputs return instructions=0, mask=0xFFFF.
 * Programs larger than the fixed bound are analyzed but never stored.
 * Renderer-thread owned; callers serialize access and program writes. */
int rsx_vp_program_cache_get(rsx_vp_program_cache* cache,
    const u8* code, u32 max_bytes, rsx_vp_program_info* out);

#endif
