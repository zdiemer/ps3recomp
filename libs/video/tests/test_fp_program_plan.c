#include "rsx_fp_program_plan.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static void word(u8* p, u32 value)
{
    value = (value << 16) | (value >> 16);
    p[0] = (u8)(value >> 24); p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8); p[3] = (u8)value;
}

static u32 random_word(void)
{
    static u32 s = 0x852a1367u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static u32 program(u8* code, u32 instructions, u32* constants, u32* count)
{
    u32 offset = 0;
    *count = 0;
    for (u32 i = 0; i < instructions; ++i) {
        const u32 has_constant = random_word() & 1u;
        word(code + offset, (random_word() & ~1u) | (i + 1u == instructions));
        for (u32 j = 1; j <= 3; ++j)
            word(code + offset + j * 4u,
                 (random_word() & ~3u) | (has_constant && j == 1u ? 2u : 0u));
        offset += 16u;
        if (has_constant) {
            constants[(*count)++] = offset;
            for (u32 j = 0; j < 4; ++j)
                word(code + offset + j * 4u, random_word());
            offset += 16u;
        }
    }
    return offset;
}

static void parity(rsx_fp_program_plan* plan, const u8* code, u32 bytes)
{
    static rsx_fp_constant_block a, b;
    const unsigned long long start = g_rsx_fp_nonfinite_constants;
    const int ca = rsx_fp_collect_constants(code, bytes, &a);
    const unsigned long long middle = g_rsx_fp_nonfinite_constants;
    const int cb = rsx_fp_program_plan_constants(plan, code, bytes, &b);
    assert(ca == cb && ca >= 0);
    assert(!memcmp(&a, &b, sizeof(a)));
    assert(middle - start == g_rsx_fp_nonfinite_constants - middle);
    assert(rsx_fp_program_plan_size(plan) == rsx_fp_program_size(code, bytes));
    for (u32 seed = 0; seed < 12; ++seed) {
        const u64 h = rsx_fp_structural_hash(code, bytes, seed);
        assert(h == rsx_fp_program_plan_hash(plan, seed));
        assert(h == rsx_fp_program_plan_hash(plan, seed));
    }
}

int main(void)
{
    static u8 code[RSX_FP_PROGRAM_PLAN_MAX_BYTES];
    static u32 offsets[RSX_FP_MAX_INLINE_CONSTANTS];
    u32 count = 0;
    for (u32 n = 1; n <= 513; ++n) {
        const u32 bytes = program(code, n, offsets, &count);
        rsx_fp_program_plan* p = rsx_fp_program_plan_create(code, bytes);
        assert(p && rsx_fp_program_plan_matches(p, code, bytes));
        parity(p, code, bytes);
        for (u32 i = 0; i < count; ++i)
            for (u32 j = 0; j < 4; ++j)
                word(code + offsets[i] + j * 4, random_word());
        assert(rsx_fp_program_plan_matches(p, code, bytes));
        parity(p, code, bytes);
        /* Every instruction byte, including END and operand type fields,
         * must invalidate the plan. Constant bytes must not invalidate it. */
        u32 constant = 0;
        for (u32 i = 0; i < bytes; ++i) {
            while (constant < count && i >= offsets[constant] + 16u)
                ++constant;
            const int in_constant = constant < count && i >= offsets[constant];
            code[i] ^= 0x80;
            assert(rsx_fp_program_plan_matches(p, code, bytes) == in_constant);
            code[i] ^= 0x80;
        }
        assert(!rsx_fp_program_plan_matches(p, code, bytes - 1));
        rsx_fp_program_plan_destroy(p);
    }
    puts("513 programs: size, hash seeds, constants, sanitization, every-byte mutation parity passed");

    memset(code, 0, sizeof(code));
    assert(!rsx_fp_program_plan_create(NULL, sizeof(code)));
    assert(!rsx_fp_program_plan_create(code, 15));
    assert(!rsx_fp_program_plan_create(code, sizeof(code))); /* no END */
    word(code, 1); word(code + 4, 2);
    assert(!rsx_fp_program_plan_create(code, 16)); /* END with missing CONST */
    rsx_fp_program_plan* p = rsx_fp_program_plan_create(code, 32);
    assert(p);
    static rsx_fp_constant_block c;
    assert(rsx_fp_program_plan_constants(p, code, 31, &c) == -1);
    assert(rsx_fp_program_plan_constants(p, code, 32, NULL) == -1);
    rsx_fp_program_plan_destroy(p);
    /* Exact maximum: 2048 constant-bearing instructions / 65536 bytes. */
    for (u32 i = 0; i < 2048; ++i) {
        word(code + i * 32u, i == 2047u ? 1u : 0u);
        word(code + i * 32u + 4u, 2u);
    }
    p = rsx_fp_program_plan_create(code, sizeof(code));
    assert(p); parity(p, code, sizeof(code));
    rsx_fp_program_plan_destroy(p);
    puts("malformed, truncated and maximum-size boundaries passed");

    const u32 bytes = program(code, 127, offsets, &count);
    rsx_fp_program_cache* cache = rsx_fp_program_cache_create();
    assert(cache);
    p = rsx_fp_program_cache_acquire(cache, 0x10000, code, bytes);
    assert(p);
    for (u32 i = 0; i < 100; ++i) {
        if (count) word(code + offsets[0], random_word());
        assert(p == rsx_fp_program_cache_acquire(cache, 0x10000, code, bytes));
        parity(p, code, bytes);
    }
    code[2] ^= 0x10; /* same storage, changed instruction */
    p = rsx_fp_program_cache_acquire(cache, 0x10000, code, bytes);
    assert(p); parity(p, code, bytes);
    rsx_fp_program_cache_stats stats = rsx_fp_program_cache_statistics(cache);
    assert(stats.hits == 100 && stats.misses == 2 && stats.mutations == 1);
    assert(!rsx_fp_program_cache_acquire(cache, 0x10000, code, 15));
    /* Exercise replacement beyond total capacity and check every result. */
    for (u32 i = 0; i < 1024; ++i) {
        p = rsx_fp_program_cache_acquire(cache, (u64)i * 512, code, bytes);
        assert(p && rsx_fp_program_plan_matches(p, code, bytes));
    }
    stats = rsx_fp_program_cache_statistics(cache);
    assert(stats.evictions && stats.rejected == 1);
    rsx_fp_program_cache_destroy(cache);
    puts("cache constants-only reuse, address mutation, malformed replacement and bounded eviction passed");

    /* Mechanism timing only: includes exact compatibility validation. This
     * is not a game frame benchmark or a claim of end-to-end savings. */
    p = rsx_fp_program_plan_create(code, bytes);
    LARGE_INTEGER freq, a, b, d;
    QueryPerformanceFrequency(&freq);
    volatile u64 consume = 0;
    QueryPerformanceCounter(&a);
    for (u32 i = 0; i < 50000; ++i) {
        consume += rsx_fp_program_size(code, bytes);
        consume += (u64)rsx_fp_collect_constants(code, bytes, &c);
        consume ^= rsx_fp_structural_hash(code, bytes, 123);
    }
    QueryPerformanceCounter(&b);
    for (u32 i = 0; i < 50000; ++i) {
        assert(rsx_fp_program_plan_matches(p, code, bytes));
        consume += rsx_fp_program_plan_size(p);
        consume += (u64)rsx_fp_program_plan_constants(p, code, bytes, &c);
        consume ^= rsx_fp_program_plan_hash(p, 123);
    }
    QueryPerformanceCounter(&d);
    printf("mechanism only: legacy_ms=%.3f plan_ms=%.3f consume=%llu\n",
        (double)(b.QuadPart-a.QuadPart)*1000/freq.QuadPart,
        (double)(d.QuadPart-b.QuadPart)*1000/freq.QuadPart,
        (unsigned long long)consume);
    rsx_fp_program_plan_destroy(p);
    puts("native FP program plan: PASS");
    return 0;
}
