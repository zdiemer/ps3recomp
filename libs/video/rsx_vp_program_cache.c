#include "rsx_vp_program_cache.h"
#include "rsx_dispatch.h"
#include "rsx_commands.h"
#include <string.h>

static u64 vp_hash(const u8* code, u32 bytes)
{
    /* Preserve the live renderer's existing seed and exact PSO identities. */
    u64 hash = 1469598103934665603ull;
    for (u32 i = 0; i < bytes; ++i) {
        hash ^= code[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static u32 read_le(const u8* p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
        ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

int rsx_vp_program_cache_get(rsx_vp_program_cache* cache,
    const u8* code, u32 max_bytes, rsx_vp_program_info* out)
{
    memset(out, 0, sizeof(*out));
    out->input_mask = 0xFFFFu;
    if (!code || max_bytes < 16u) return 0;
    const u32 set = (u32)vp_hash(code, 16u) & (RSX_VP_CACHE_SETS - 1u);
    if (cache) {
        for (u32 way = 0; way < RSX_VP_CACHE_WAYS; ++way) {
            const rsx_vp_program_entry* entry = &cache->entries[set][way];
            const u32 bytes = entry->info.instructions * 16u;
            if (bytes && bytes <= max_bytes &&
                memcmp(code, entry->bytes, bytes) == 0) {
                *out = entry->info;
                return 1;
            }
        }
    }
    out->instructions = rsx_vp_program_size_instrs(code, max_bytes);
    if (!out->instructions) return 0;
    const u32 bytes = out->instructions * 16u;
    rsx_vp_input_analysis analysis = {0xFFFFu, 0};
    if (rsx_vp_analyze_inputs(code, bytes, &analysis) == (int)out->instructions &&
        analysis.exact)
        out->input_mask = analysis.input_mask;
    for (u32 i = 0; i < out->instructions; ++i) {
        const u8* p = code + i * 16u;
        if (((read_le(p + 4) >> 22) & 0x1Fu) == 0x19u)
            out->txl_mask |= 1u << ((read_le(p + 8) >> 8) & 3u);
    }
    out->hash = vp_hash(code, bytes);
    if (cache && bytes <= RSX_VP_CACHE_MAX_BYTES) {
        const u32 way = cache->next[set]++ % RSX_VP_CACHE_WAYS;
        rsx_vp_program_entry* entry = &cache->entries[set][way];
        memcpy(entry->bytes, code, bytes);
        entry->info = *out;
    }
    return 0;
}

int rsx_vp_mirror_cache_get(rsx_vp_mirror_cache* memo,
    rsx_vp_program_cache* cache, rsx_dispatch* rsx, rsx_vp_program_info* out)
{
    /* Same live register as rsx_dsp_vp_start; no guest pointer or renderer
     * dependency is part of the memo identity. */
    const u32 start = rsx->regs[NV4097_SET_TRANSFORM_PROGRAM_START >> 2];
    if (memo->valid && !rsx->vp_metadata_dirty && memo->start == start) {
        *out = memo->info;
        return 2;
    }
    int hit = 0;
    memset(out, 0, sizeof(*out));
    out->input_mask = 0xFFFFu;
    if (start < RSX_DSP_VP_INSTR)
        hit = rsx_vp_program_cache_get(cache, (const u8*)(rsx->vp + start * 4u),
            (RSX_DSP_VP_INSTR - start) * 16u, out);
    memo->info = *out;
    memo->start = start;
    memo->valid = 1;
    rsx->vp_metadata_dirty = 0;
    return hit;
}
