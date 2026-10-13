#include "rsx_vp_burst.h"

/* Standalone parity build (from the ps3recomp root; output path is arbitrary):
 * clang-cl /nologo /std:c11 /O2 /Gy /Gw /D_CRT_SECURE_NO_WARNINGS
 *   /I include /I libs/video libs/video/tests/test_rsx_vp_burst.c
 *   libs/video/rsx_vp_burst.c libs/video/rsx_dispatch.c
 *   libs/video/rsx_commands.c /link /OPT:REF /OUT:test_rsx_vp_burst.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Link-only stubs for unrelated branches in the unchanged SDK handler. The VP
 * tests never exercise these runtime side effects. */
uint32_t ppu_hle_inject_base = 0;
void vm_write32(uint64_t address, uint32_t value)
{
    (void)address;
    (void)value;
    abort();
}
void ps3_ms(const char *key) { (void)key; abort(); }

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return 1; \
} } while (0)

static void fill_words(u32 words[RSX_VP_BURST_DWORDS], u32 seed)
{
    static const u32 edge[] = {
        0x01020304u, 0xAABBCCDDu, 0x80000001u, 0x00000000u,
        0xFFFFFFFFu, 0x7F800000u, 0xDEADBEEFu, 0x00010203u,
        0x04030201u, 0xFF00AA55u, 0x13579BDFu, 0x2468ACE0u
    };
    for (u32 i = 0; i < RSX_VP_BURST_DWORDS; ++i)
        words[i] = edge[(i + seed) % (sizeof(edge) / sizeof(edge[0]))] ^
                   (0x9E3779B9u * (i + seed));
    words[0] = 0x01020304u ^ seed;
    words[1] = 0xAABBCCDDu ^ seed;
}

static int test_dispatch(void)
{
    u32 words[RSX_VP_BURST_DWORDS];
    u32 case_count = 0;
    for (u32 pattern = 0; pattern < 4; ++pattern) {
        fill_words(words, pattern);
        for (u32 load = 0; load < RSX_DSP_VP_INSTR; ++load) {
            rsx_dispatch generic, fast;
            rsx_dispatch_init(&generic, NULL);
            generic.regs[0x1E9Cu >> 2] = load;
            generic.in_begin_end = (int)((pattern + load) & 1u);
            generic.current_primitive = 0xA500u + pattern;
            generic.inline_len = 123u + load;
            generic.inline_dropped = 7u + pattern;
            for (u32 i = 0; i < RSX_DSP_NUM_REGS; ++i) {
                generic.seen[i] = i * 13u + pattern;
                generic.klass[i] = (u8)(i + load);
            }
            for (u32 i = 0; i < RSX_VP_BURST_DWORDS; ++i)
                generic.regs[(RSX_VP_BURST_METHOD >> 2) + i] = 0x55550000u + i;
            memcpy(&fast, &generic, sizeof(fast));

            for (u32 i = 0; i < RSX_VP_BURST_DWORDS; ++i)
                rsx_dispatch_method(&generic, RSX_VP_BURST_METHOD + 4u * i, words[i]);
            CHECK(rsx_vp_burst_dispatch_32(&fast, 0, RSX_VP_BURST_METHOD, words) == 1);
            CHECK(memcmp(&generic, &fast, sizeof(fast)) == 0);
            ++case_count;
        }
        static const u32 near_boundary[] = {
            RSX_DSP_VP_INSTR, RSX_DSP_VP_INSTR + 1u,
            0x40000000u, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu
        };
        for (u32 c = 0; c < sizeof(near_boundary) / sizeof(near_boundary[0]); ++c) {
            rsx_dispatch generic, fast;
            rsx_dispatch_init(&generic, NULL);
            generic.regs[0x1E9Cu >> 2] = near_boundary[c];
            generic.in_begin_end = (int)((pattern + c) & 1u);
            for (u32 i = 0; i < RSX_DSP_NUM_REGS; ++i) {
                generic.seen[i] = i * 13u + pattern;
                generic.klass[i] = (u8)(i + c);
            }
            memcpy(&fast, &generic, sizeof(fast));
            for (u32 i = 0; i < RSX_VP_BURST_DWORDS; ++i)
                rsx_dispatch_method(&generic, RSX_VP_BURST_METHOD + 4u * i, words[i]);
            CHECK(rsx_vp_burst_dispatch_32(&fast, 0, RSX_VP_BURST_METHOD, words) == 1);
            CHECK(memcmp(&generic, &fast, sizeof(fast)) == 0);
            ++case_count;
        }
    }

    rsx_dispatch s, before;
    rsx_dispatch_init(&s, NULL);
    memcpy(&before, &s, sizeof(s));
    u32 words2[RSX_VP_BURST_DWORDS] = {0};
    CHECK(rsx_vp_burst_dispatch_32(&s, 1, RSX_VP_BURST_METHOD, words2) == 0);
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);
    CHECK(rsx_vp_burst_dispatch_32(&s, 0, (RSX_VP_BURST_METHOD + 4u), words2) == 0);
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);
    CHECK(rsx_vp_burst_dispatch_32(&s, 0, RSX_VP_BURST_METHOD, NULL) == 0);
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);
    CHECK(rsx_vp_burst_dispatch_32(NULL, 0, RSX_VP_BURST_METHOD, words2) == 0);
    printf("dispatch cases: %u\n", case_count);
    return 0;
}

static int test_state(void)
{
    u32 words[RSX_VP_BURST_DWORDS];
    const u32 capacity = (u32)sizeof(((rsx_state *)0)->vp_ucode);
    u32 case_count = 0;
    for (u32 pattern = 0; pattern < 4; ++pattern) {
        fill_words(words, pattern * 3u);
        for (u32 cursor = 0; cursor <= capacity; cursor += 4u) {
            rsx_state generic, fast;
            rsx_state_init(&generic);
            generic.vp_ucode_write = cursor;
            generic.vp_ucode_bytes = (pattern * 97u) % sizeof(generic.vp_ucode);
            generic.vp_dirty = (int)((pattern + cursor) & 1u);
            generic.transform_program_load = 0x12340000u + pattern;
            generic.transform_program_start = 0xABCD0000u + cursor;
            generic.shader_dirty = (int)(pattern & 1u);
            generic.in_begin_end = (int)((pattern + cursor) & 1u);
            for (u32 i = 0; i < sizeof(generic.vp_ucode); ++i)
                generic.vp_ucode[i] = (u8)(i * 17u + pattern + cursor);
            memcpy(&fast, &generic, sizeof(fast));

            for (u32 i = 0; i < RSX_VP_BURST_DWORDS; ++i)
                CHECK(rsx_process_method(&generic, RSX_VP_BURST_METHOD + 4u * i, words[i]) == 0);
            CHECK(rsx_vp_burst_process_32(&fast, 0, RSX_VP_BURST_METHOD, words) == 1);
            CHECK(memcmp(&generic, &fast, sizeof(fast)) == 0);
            ++case_count;
        }
        /* Exercise every byte offset in the tail around the capacity edge. */
        for (u32 cursor = capacity - 7u; cursor <= capacity + 3u; ++cursor) {
            rsx_state generic, fast;
            rsx_state_init(&generic);
            generic.vp_ucode_write = cursor;
            generic.vp_ucode_bytes = pattern * 97u;
            generic.vp_dirty = (int)((pattern + cursor) & 1u);
            generic.in_begin_end = (int)((pattern + cursor) & 1u);
            for (u32 i = 0; i < sizeof(generic.vp_ucode); ++i)
                generic.vp_ucode[i] = (u8)(i * 17u + pattern + cursor);
            memcpy(&fast, &generic, sizeof(fast));
            for (u32 i = 0; i < RSX_VP_BURST_DWORDS; ++i)
                CHECK(rsx_process_method(&generic, RSX_VP_BURST_METHOD + 4u * i, words[i]) == 0);
            CHECK(rsx_vp_burst_process_32(&fast, 0, RSX_VP_BURST_METHOD, words) == 1);
            CHECK(memcmp(&generic, &fast, sizeof(fast)) == 0);
            ++case_count;
        }
    }

    rsx_state s, before;
    rsx_state_init(&s);
    memcpy(&before, &s, sizeof(s));
    u32 words2[RSX_VP_BURST_DWORDS] = {0};
    CHECK(rsx_vp_burst_process_32(&s, 1, RSX_VP_BURST_METHOD, words2) == 0);
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);
    CHECK(rsx_vp_burst_process_32(&s, 0, (RSX_VP_BURST_METHOD + 4u), words2) == 0);
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);
    CHECK(rsx_vp_burst_process_32(&s, 0, RSX_VP_BURST_METHOD, NULL) == 0);
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);
    CHECK(rsx_vp_burst_process_32(NULL, 0, RSX_VP_BURST_METHOD, words2) == 0);
    /* The native boundary rejects wrapped cursors. The generic fallback's
     * nonwrapping capacity check must also leave state untouched. */
    static const u32 wrap_cursors[] = {
        UINT32_MAX - 3u, UINT32_MAX - 2u, UINT32_MAX - 1u, UINT32_MAX
    };
    for (u32 i = 0; i < sizeof(wrap_cursors) / sizeof(wrap_cursors[0]); ++i) {
        rsx_state unsafe;
        rsx_state_init(&unsafe);
        unsafe.vp_ucode_write = wrap_cursors[i];
        memcpy(&before, &unsafe, sizeof(before));
        CHECK(rsx_vp_burst_process_32(&unsafe, 0, RSX_VP_BURST_METHOD, words2) == 0);
        CHECK(memcmp(&unsafe, &before, sizeof(unsafe)) == 0);
        CHECK(rsx_process_method(&unsafe,RSX_VP_BURST_METHOD,0x12345678u)==0);
        CHECK(memcmp(&unsafe, &before, sizeof(unsafe)) == 0);
    }
    printf("rsx_state cases: %u; unsafe-wrap rejections: 4\n", case_count);
    return 0;
}

int main(void)
{
    CHECK(test_dispatch() == 0);
    CHECK(test_state() == 0);
    puts("VP 32-word consumer parity: PASS (generic SDK handlers, complete structs)");
    return 0;
}
