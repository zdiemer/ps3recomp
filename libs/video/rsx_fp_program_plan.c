#include "rsx_fp_program_plan.h"

#include <stdlib.h>
#include <string.h>

enum { FP_PLAN_HASH_SLOTS = 8 };

struct rsx_fp_program_plan {
    u32 size, instruction_count, constant_count, next_hash;
    u32 constant_offset[RSX_FP_MAX_INLINE_CONSTANTS];
    struct { u64 seed, hash; int valid; } hashes[FP_PLAN_HASH_SLOTS];
    u8 code[RSX_FP_PROGRAM_PLAN_MAX_BYTES];
};

rsx_fp_program_plan* rsx_fp_program_plan_create(const u8* code, u32 bytes)
{
    if (!code || bytes < 16u)
        return NULL;
    if (bytes > RSX_FP_PROGRAM_PLAN_MAX_BYTES)
        bytes = RSX_FP_PROGRAM_PLAN_MAX_BYTES;
    rsx_fp_program_plan* p = (rsx_fp_program_plan*)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    u32 offset = 0;
    while (bytes - offset >= 16u) {
        const u32 w0 = rsx_fp_read_word(code + offset);
        const u32 w1 = rsx_fp_read_word(code + offset + 4);
        const u32 w2 = rsx_fp_read_word(code + offset + 8);
        const u32 w3 = rsx_fp_read_word(code + offset + 12);
        memcpy(p->code + offset, code + offset, 16);
        ++p->instruction_count;
        offset += 16;
        /* NV40 source register type is the low two bits. This intentionally
         * follows the existing collector even for branch instructions. */
        if ((w1 & 3u) == 2u || (w2 & 3u) == 2u || (w3 & 3u) == 2u) {
            if (bytes - offset < 16u ||
                p->constant_count == RSX_FP_MAX_INLINE_CONSTANTS)
                break;
            p->constant_offset[p->constant_count++] = offset;
            offset += 16;
        }
        if (w0 & 1u) {
            p->size = offset;
            return p;
        }
    }
    free(p);
    return NULL;
}

void rsx_fp_program_plan_destroy(rsx_fp_program_plan* plan)
{
    free(plan);
}

u32 rsx_fp_program_plan_size(const rsx_fp_program_plan* plan)
{
    return plan ? plan->size : 0;
}

int rsx_fp_program_plan_matches(
    const rsx_fp_program_plan* p, const u8* code, u32 bytes)
{
    if (!p || !code || bytes < p->size)
        return 0;
    u32 offset = 0;
    for (u32 i = 0; i < p->constant_count; ++i) {
        const u32 end = p->constant_offset[i];
        if (memcmp(p->code + offset, code + offset, end - offset))
            return 0;
        offset = end + 16u;
    }
    return !memcmp(p->code + offset, code + offset, p->size - offset);
}

int rsx_fp_program_plan_constants(const rsx_fp_program_plan* p,
    const u8* code, u32 bytes, rsx_fp_constant_block* out)
{
    if (!p || !code || !out || bytes < p->size)
        return -1;
    memset(out, 0, sizeof(*out));
    for (u32 i = 0; i < p->constant_count; ++i)
        for (u32 j = 0; j < 4u; ++j)
            out->values[i][j] = rsx_fp_sanitize_constant_word(
                rsx_fp_read_word(code + p->constant_offset[i] + j * 4u));
    out->count = p->constant_count;
    return (int)p->constant_count;
}

u64 rsx_fp_program_plan_hash(rsx_fp_program_plan* p, u64 seed)
{
    if (!p)
        return 0;
    for (u32 i = 0; i < FP_PLAN_HASH_SLOTS; ++i)
        if (p->hashes[i].valid && p->hashes[i].seed == seed)
            return p->hashes[i].hash;
    const u64 hash = rsx_fp_structural_hash(p->code, p->size, seed);
    const u32 slot = p->next_hash++ % FP_PLAN_HASH_SLOTS;
    p->hashes[slot].seed = seed;
    p->hashes[slot].hash = hash;
    p->hashes[slot].valid = 1;
    return hash;
}

enum { FP_PLAN_CACHE_SETS = 32, FP_PLAN_CACHE_WAYS = 4 };
typedef struct fp_plan_cache_entry {
    u64 storage_key, age;
    rsx_fp_program_plan* plan;
} fp_plan_cache_entry;

struct rsx_fp_program_cache {
    fp_plan_cache_entry entries[FP_PLAN_CACHE_SETS][FP_PLAN_CACHE_WAYS];
    rsx_fp_program_cache_stats stats;
    u64 age;
};

rsx_fp_program_cache* rsx_fp_program_cache_create(void)
{
    return (rsx_fp_program_cache*)calloc(1, sizeof(rsx_fp_program_cache));
}

void rsx_fp_program_cache_destroy(rsx_fp_program_cache* cache)
{
    if (!cache)
        return;
    for (u32 i = 0; i < FP_PLAN_CACHE_SETS; ++i)
        for (u32 j = 0; j < FP_PLAN_CACHE_WAYS; ++j)
            rsx_fp_program_plan_destroy(cache->entries[i][j].plan);
    free(cache);
}

rsx_fp_program_plan* rsx_fp_program_cache_acquire(rsx_fp_program_cache* cache,
    u64 key, const u8* code, u32 bytes)
{
    if (!cache || !code)
        return NULL;
    /* Select a bounded set from the offset. The full key, including memory
     * location, is always compared below before reusing a plan. */
    const u64 folded = key ^ (key >> 16) ^ (key >> 32);
    fp_plan_cache_entry* set = cache->entries[(folded >> 4) % FP_PLAN_CACHE_SETS];
    fp_plan_cache_entry* victim = NULL;
    for (u32 i = 0; i < FP_PLAN_CACHE_WAYS; ++i) {
        fp_plan_cache_entry* e = &set[i];
        if (e->plan && e->storage_key == key) {
            if (rsx_fp_program_plan_matches(e->plan, code, bytes)) {
                ++cache->stats.hits;
                e->age = ++cache->age;
                return e->plan;
            }
            victim = e;
            ++cache->stats.mutations;
            break;
        }
        if (!victim || (!e->plan && victim->plan) ||
            (e->plan && victim->plan && e->age < victim->age))
            victim = e;
    }
    ++cache->stats.misses;
    rsx_fp_program_plan* plan = rsx_fp_program_plan_create(code, bytes);
    if (!plan) {
        ++cache->stats.rejected;
        return NULL;
    }
    if (victim->plan && victim->storage_key != key)
        ++cache->stats.evictions;
    rsx_fp_program_plan_destroy(victim->plan);
    victim->plan = plan;
    victim->storage_key = key;
    victim->age = ++cache->age;
    return plan;
}

rsx_fp_program_cache_stats rsx_fp_program_cache_statistics(
    const rsx_fp_program_cache* cache)
{
    const rsx_fp_program_cache_stats empty = {0};
    return cache ? cache->stats : empty;
}
