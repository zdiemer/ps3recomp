#ifndef PS3RECOMP_RSX_FP_PROGRAM_PLAN_H
#define PS3RECOMP_RSX_FP_PROGRAM_PLAN_H

#include "rsx_fp_decompiler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An owned, immutable instruction image plus a predecoded constant gather
 * plan. Inline constant values do not belong to program identity. A plan may
 * be used directly by a producer that owns code generations; legacy callers
 * must first match the current instruction bytes, not just the guest address.
 * Hash memoization makes an instance single-thread-owned. */
typedef struct rsx_fp_program_plan rsx_fp_program_plan;

#define RSX_FP_PROGRAM_PLAN_MAX_BYTES 65536u

rsx_fp_program_plan* rsx_fp_program_plan_create(const u8* code, u32 bytes);
void rsx_fp_program_plan_destroy(rsx_fp_program_plan* plan);
u32 rsx_fp_program_plan_size(const rsx_fp_program_plan* plan);

/* Exact comparison of every instruction byte, excluding constant payloads.
 * A successful check is valid only while the input version stays stable. */
int rsx_fp_program_plan_matches(
    const rsx_fp_program_plan* plan, const u8* code, u32 bytes);

/* The caller supplies the constant storage for this code version. This does
 * not rescan instructions; caller must own the version or have matched it.
 * Output (including unused slots) and non-finite accounting match the legacy
 * collector. Returns count, or -1 for invalid/truncated storage. */
int rsx_fp_program_plan_constants(const rsx_fp_program_plan* plan,
    const u8* code, u32 bytes, rsx_fp_constant_block* out);

/* Bit-identical to rsx_fp_structural_hash for every seed. Repeated VP/FP
 * pairings reuse the result without hashing instructions again. */
u64 rsx_fp_program_plan_hash(rsx_fp_program_plan* plan, u64 seed);

/* Bounded compatibility adapter for mutable guest storage. Each acquisition
 * checks instruction bytes exactly; a recycled address never implies reuse.
 * Returned plans live until the next acquisition or cache destruction. */
typedef struct rsx_fp_program_cache rsx_fp_program_cache;
typedef struct rsx_fp_program_cache_stats {
    u64 hits, misses, mutations, evictions, rejected;
} rsx_fp_program_cache_stats;
rsx_fp_program_cache* rsx_fp_program_cache_create(void);
void rsx_fp_program_cache_destroy(rsx_fp_program_cache* cache);
rsx_fp_program_plan* rsx_fp_program_cache_acquire(rsx_fp_program_cache* cache,
    u64 storage_key, const u8* code, u32 bytes);
rsx_fp_program_cache_stats rsx_fp_program_cache_statistics(
    const rsx_fp_program_cache* cache);

#ifdef __cplusplus
}
#endif
#endif
