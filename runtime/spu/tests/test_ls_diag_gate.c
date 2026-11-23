/* Local-store diagnostic gate: the unchecked accessors must match the checked
 * ones byte for byte, the gate must clear only after every probe gate resolved
 * off, and any armed probe (env or image-2 policy) must keep it on. */
#include <assert.h>
#include "../spu_context.h"

int g_spu_ls_watch_n = -1;
static unsigned watch_calls;
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{
    (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr;
    if (g_spu_ls_watch_n < 0) g_spu_ls_watch_n = getenv("SPU_LS_WATCH") ? 1 : 0;
    if (g_spu_ls_watch_n) ++watch_calls;
}

static spu_context ctx;
static u128 pattern(uint32_t seed)
{
    u128 v;
    for (int i = 0; i < 4; i++) v._u32[i] = seed * 0x9E3779B9u + (uint32_t)i * 0x01020304u;
    return v;
}
/* LS holds SPU (big-endian) bytes; misaligned and out-of-range addresses
 * address the containing 16-byte line within the 256 KB store. */
static void roundtrip(uint32_t lsa, uint32_t seed)
{
    u128 v = pattern(seed);
    spu_ls_write128(&ctx, lsa, v);
    const uint8_t* p = &ctx.ls[lsa & SPU_LS_MASK & ~0xFu];
    for (int i = 0; i < 16; i++) assert(p[i] == (uint8_t)(v._u32[i / 4] >> (24 - (i % 4) * 8)));
    u128 r = spu_ls_read128(&ctx, lsa);
    assert(!memcmp(&r, &v, 16));
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    ctx.ls = ctx.ls_store;
    ctx.image_id = 33;
    assert(g_spu_ls_diag != 0 && g_spu_ls_probe < 0 && g_spu_smc_watch < 0);
    if (!strcmp(argv[1], "off")) {
        /* The first checked read and write resolve every gate, then clear it. */
        roundtrip(0x1230, 1);
        assert(g_spu_ls_probe == 0 && g_spu_smc_watch == 0 && g_spu_ls_watch_n == 0);
        assert(g_spu_ls_diag == 0);
        for (uint32_t i = 0; i < 4096; i++) roundtrip(i * 0x47u + 0xFFFC0000u * (i & 1), i + 2);
        /* Unchecked accesses match the checked ones on the same bytes. */
        u128 fast = spu_ls_read128(&ctx, 0x3FFF8);
        g_spu_ls_diag = 1;
        u128 slow = spu_ls_read128(&ctx, 0x3FFF8);
        assert(!memcmp(&fast, &slow, 16) && g_spu_ls_diag == 0);
        /* A policy context running image 2 keeps the WWS probes live. */
        g_spu_ls_policy_probe = 1;
        g_spu_ls_diag = 1;
        roundtrip(0x100, 9);
        assert(g_spu_ls_diag == 1);
    } else if (!strcmp(argv[1], "watch")) {
        /* SPU_LS_WATCH: every read and write keeps reaching the watch. */
        roundtrip(0x200, 1);
        assert(g_spu_ls_watch_n == 1 && g_spu_ls_diag == 1);
        unsigned before = watch_calls;
        for (uint32_t i = 0; i < 100; i++) roundtrip(i * 16, i);
        assert(watch_calls == before + 200);
    } else if (!strcmp(argv[1], "smc")) {
        /* SPU_SMC_WATCH resolves on the first write and keeps the gate on. */
        roundtrip(0x200, 1);
        assert(g_spu_smc_watch == 1 && g_spu_ls_diag == 1);
    } else if (!strcmp(argv[1], "lowread")) {
        u128 r = spu_ls_read128(&ctx, 0x40);
        (void)r;
        assert(g_spu_ls_probe == 1 && g_spu_ls_diag == 1);
    } else {
        return 2;
    }
    printf("LS diagnostic gate %s passed\n", argv[1]);
    return 0;
}
