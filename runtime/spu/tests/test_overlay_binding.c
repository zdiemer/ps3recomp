#include <assert.h>
#include <stdio.h>
#include "../spu_overlay_registry.c"

static uint64_t fingerprint(const uint8_t* bytes, uint32_t span)
{
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < span; ++i) h = (h ^ bytes[i]) * 1099511628211ULL;
    return h;
}
int main(void)
{
    uint8_t a[128] = {0}, b[128] = {0}, unknown[128] = {0};
    a[127] = 1; b[127] = 2; unknown[127] = 3;
    /* Identical initial instructions must not confuse two overlay programs. */
    spu_overlay_register_fingerprint(fingerprint(a, sizeof a), sizeof a, 45);
    spu_overlay_register_fingerprint(fingerprint(b, sizeof b), sizeof b, 46);
    spu_overlay_bind(0x10000, a, sizeof a);
    spu_overlay_bind(0x20000, b, sizeof b);
    uint32_t span = 0;
    assert(spu_overlay_lookup_bound(0x10000, &span) == 45 && span == 128);
    assert(spu_overlay_lookup_bound(0x20000, &span) == 46);
    assert(spu_overlay_lookup_bound(0x10010, &span) == 0);
    spu_overlay_bind(0x30000, a, sizeof a);
    assert(spu_overlay_lookup_bound(0x30000, &span) == 45);
    spu_overlay_bind(0x10000, b, sizeof b);
    assert(spu_overlay_lookup_bound(0x10000, &span) == 46);
    spu_overlay_bind(0x10000, unknown, sizeof unknown);
    assert(spu_overlay_lookup_bound(0x10000, &span) == 0);
    /* Partial overwrite invalidates the previous whole-program binding. */
    spu_overlay_bind(0x20040, unknown, 16);
    assert(spu_overlay_lookup_bound(0x20000, &span) == 0);
    spu_overlay_bind(0xFFFFFFF0, a, sizeof a);
    assert(spu_overlay_lookup_bound(0xFFFFFFF0, &span) == 0);
    puts("SPU overlay content, relocation and staging reuse checks passed");
    return 0;
}
