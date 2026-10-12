#include "../rsx_vp_program_cache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#endif

static u32 rng = 0x6519CA73u;
static u32 random_word(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static u32 rd(const u8* p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
        ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void oracle(const u8* code, u32 bound, rsx_vp_program_info* out)
{
    memset(out, 0, sizeof(*out)); out->input_mask = 0xFFFFu;
    out->instructions = rsx_vp_program_size_instrs(code, bound);
    if (!out->instructions) return;
    rsx_vp_input_analysis inputs;
    const u32 bytes = out->instructions * 16u;
    if (rsx_vp_analyze_inputs(code, bytes, &inputs) == (int)out->instructions && inputs.exact)
        out->input_mask = inputs.input_mask;
    out->hash = 1469598103934665603ull;
    for (u32 i = 0; i < bytes; ++i)
        out->hash = (out->hash ^ code[i]) * 1099511628211ull;
    for (u32 i = 0; i < bytes; i += 16u)
        if (((rd(code + i + 4) >> 22) & 31u) == 25u)
            out->txl_mask |= 1u << ((rd(code + i + 8) >> 8) & 3u);
}

static int check(rsx_vp_program_cache* cache, const u8* code, u32 bound)
{
    rsx_vp_program_info expected, actual;
    oracle(code, bound, &expected);
    const int hit = rsx_vp_program_cache_get(cache, code, bound, &actual);
    assert(expected.instructions == actual.instructions);
    assert(expected.input_mask == actual.input_mask);
    assert(expected.txl_mask == actual.txl_mask);
    assert(expected.hash == actual.hash);
    return hit;
}

int main(void)
{
    rsx_vp_program_cache* cache = calloc(1, sizeof(*cache));
    assert(cache);
    u8 code[RSX_VP_CACHE_MAX_BYTES + 16u], copy[sizeof(code)];
    /* A live input, all four TXL units, and an unsupported operation exercise
     * metadata changes explicitly, beyond the randomized byte oracle. */
    memset(code, 0, sizeof(code));
    u32 words[4] = {0, (1u << 22) | (3u << 8), 2u << 23, (15u << 13) | 1u};
    memcpy(code, words, sizeof(words));
    check(cache, code, sizeof(words));
    rsx_vp_program_info info;
    assert(rsx_vp_program_cache_get(cache, code, sizeof(words), &info));
    assert(info.input_mask == (1u << 3));
    for (u32 unit = 0; unit < 4; ++unit) {
        words[1] = 25u << 22; words[2] = unit << 8;
        memcpy(code, words, sizeof(words));
        check(cache, code, sizeof(words));
        assert(rsx_vp_program_cache_get(cache, code, sizeof(words), &info));
        assert(info.txl_mask == (1u << unit));
    }
    words[1] = 31u << 22;
    memcpy(code, words, sizeof(words));
    check(cache, code, sizeof(words));
    rsx_vp_program_cache_get(cache, code, sizeof(words), &info);
    assert(info.input_mask == 0xFFFFu);
    unsigned mutations = 0;
    for (u32 test = 0; test < 32u; ++test) {
        memset(cache, 0, sizeof(*cache));
        const u32 instructions = test == 31u ? 544u : 1u + random_word() % 544u;
        const u32 bytes = instructions * 16u;
        for (u32 i = 0; i < bytes; ++i) code[i] = (u8)random_word();
        for (u32 i = 0; i < instructions; ++i) code[i * 16u + 12u] &= ~1u;
        code[bytes - 4u] |= 1u;
        assert(!check(cache, code, bytes));
        assert(check(cache, code, bytes));
        memcpy(copy, code, bytes);
        assert(check(cache, copy, bytes)); /* address is not an identity */
        for (u32 i = 0; i < bytes; ++i) {
            code[i] ^= (u8)(1u << (i % 8u));
            check(cache, code, bytes);
            code[i] ^= (u8)(1u << (i % 8u));
            check(cache, code, bytes);
            ++mutations;
        }
        for (u32 bound = 0; bound < bytes; bound += 13u) check(cache, code, bound);
    }
    memset(cache, 0, sizeof(*cache));
    memset(code, 0, sizeof(code));
    code[44] = 1; /* three instructions */
    assert(!check(cache, code, sizeof(code)));
    assert(check(cache, code, sizeof(code)));
    code[28] = 1; /* earlier END must invalidate the longer snapshot */
    assert(!check(cache, code, sizeof(code)));
    code[28] = code[44] = 0; code[60] = 1; /* later END */
    assert(!check(cache, code, sizeof(code)));
    memset(cache, 0, sizeof(*cache));
    for (u32 variant = 0; variant < 20u; ++variant) {
        code[16] = (u8)variant; /* same prefix/set, more entries than ways */
        check(cache, code, sizeof(code));
        assert(check(cache, code, sizeof(code)));
    }
    memset(code, 0, sizeof(code)); code[sizeof(code) - 4u] = 1;
    assert(!check(cache, code, sizeof(code))); /* beyond cache storage */
    assert(!check(cache, code, sizeof(code)));
    check(NULL, code, sizeof(code));
    check(cache, NULL, 0);
#if defined(_WIN32)
    SYSTEM_INFO system; GetSystemInfo(&system);
    u8* pages = VirtualAlloc(NULL, system.dwPageSize * 2u,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(pages);
    DWORD old;
    assert(VirtualProtect(pages + system.dwPageSize, system.dwPageSize, PAGE_NOACCESS, &old));
    for (u32 length = 0; length <= 512u; ++length) {
        u8* tail = pages + system.dwPageSize - length;
        memset(tail, 0, length);
        if (length >= 16u) tail[(length / 16u) * 16u - 4u] = 1;
        check(cache, tail, length);
        check(cache, tail, length);
    }
    VirtualFree(pages, 0, MEM_RELEASE);
#endif
    free(cache);
    printf("VP cache passed %u instruction-byte mutations, bounded tails, END changes, content/address reuse and set eviction.\n", mutations);
    return 0;
}
