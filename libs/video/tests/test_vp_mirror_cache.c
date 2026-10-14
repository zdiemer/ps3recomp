/* Actual dispatcher, burst writer and metadata cache. Standalone from SDK:
 * clang-cl /nologo /std:c11 /O2 /I include /I libs/video
 *   libs/video/tests/test_vp_mirror_cache.c libs/video/rsx_dispatch.c
 *   libs/video/rsx_vp_burst.c libs/video/rsx_vp_program_cache.c
 *   libs/video/rsx_vp_decompiler.c /Fe:test_vp_mirror_cache.exe
 * No GPU or game assets are used. */
#include "../rsx_dispatch.h"
#include "../rsx_vp_burst.h"
#include "../rsx_vp_program_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static rsx_dispatch owner, generic;
static rsx_vp_program_cache content;
static rsx_vp_mirror_cache memo;

static u32 read_le(const u8* p)
{
    return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Original analysis/hash rules, independently of both cache lookup paths. */
static rsx_vp_program_info original_info(void)
{
    rsx_vp_program_info result = {0};
    const u32 start = rsx_dsp_vp_start(&owner);
    result.input_mask = 0xFFFFu;
    if (start < RSX_DSP_VP_INSTR) {
        const u8* code = (const u8*)(owner.vp + start * 4u);
        result.instructions = rsx_vp_program_size_instrs(code,
            (RSX_DSP_VP_INSTR - start) * 16u);
        if (result.instructions) {
            const u32 bytes = result.instructions * 16u;
            rsx_vp_input_analysis analysis;
            if (rsx_vp_analyze_inputs(code, bytes, &analysis) == (int)result.instructions &&
                    analysis.exact) result.input_mask = analysis.input_mask;
            result.hash = 1469598103934665603ull;
            for (u32 i = 0; i < bytes; ++i)
                result.hash = (result.hash ^ code[i]) * 1099511628211ull;
            for (u32 i = 0; i < bytes; i += 16u)
                if (((read_le(code + i + 4u) >> 22) & 31u) == 25u)
                    result.txl_mask |= 1u << ((read_le(code + i + 8u) >> 8) & 3u);
        }
    }
    return result;
}

static int check(void)
{
    const rsx_vp_program_info expected = original_info();
    rsx_vp_program_info actual;
    const int hit = rsx_vp_mirror_cache_get(&memo, &content, &owner, &actual);
    assert(actual.instructions == expected.instructions);
    assert(actual.input_mask == expected.input_mask);
    assert(actual.txl_mask == expected.txl_mask);
    assert(actual.hash == expected.hash);
    assert(!owner.vp_metadata_dirty);
    return hit;
}

static void write_word(u32 offset, u32 value)
{
    rsx_dispatch_method(&owner, 0x1E9Cu, offset / 4u);
    rsx_dispatch_method(&owner, 0x0B80u + (offset & 3u) * 4u, value);
}

int main(void)
{
    u32 words[32] = {0};
    words[1] = (1u << 22) | (3u << 8); /* MOV input3 */
    words[2] = 2u << 23;
    words[3] = (15u << 13) | 1u;
    rsx_dispatch_init(&owner, NULL);
    assert(owner.vp_metadata_dirty);
    rsx_dispatch_seed_transform_program(&owner, words, 32);
    /* A content-cache miss is a fully analyzed result, not invalid metadata. */
    assert(check() == 0);
    assert(memo.info.instructions == 1 && memo.info.input_mask == (1u << 3));
    assert(memo.info.hash && !memo.info.txl_mask);
    assert(check() == 2); /* layout then get_pso on an unchanged mirror */

    write_word(1, words[1]);
    assert(!owner.vp_metadata_dirty && check() == 2); /* identical partial write */
    words[1] = (1u << 22) | (7u << 8);
    write_word(1, words[1]);
    assert(owner.vp_metadata_dirty && check() == 0);
    assert(memo.info.input_mask == (1u << 7));
    assert(check() == 2);
    rsx_dispatch_seed_transform_program(&owner, words, 2);
    assert(!owner.vp_metadata_dirty && check() == 2); /* identical seed prefix */
    words[1] = (1u << 22) | (9u << 8);
    rsx_dispatch_seed_transform_program(&owner, words, 2);
    assert(owner.vp_metadata_dirty && check() == 0);
    assert(memo.info.input_mask == (1u << 9));

    /* Partial upload removes END, then repairs it later. Cache the invalid
     * result safely, and invalidate it when a later END word arrives. */
    words[3] &= ~1u;
    write_word(3, words[3]);
    assert(check() == 0 && !memo.info.instructions && memo.info.input_mask == 0xFFFFu);
    assert(check() == 2);
    words[5] = 25u << 22; words[6] = 2u << 8; /* TXL unit2 */
    rsx_dispatch_seed_transform_program(&owner, words, 7);
    check();
    words[11] = 1u;
    write_word(11, words[11]);
    check();
    assert(memo.info.instructions == 3 && memo.info.txl_mask == 4);
    words[7] = 1u; write_word(7, 1u); check();
    assert(memo.info.instructions == 2); /* earlier END */
    words[7] = 0; write_word(7, 0); check();
    assert(memo.info.instructions == 3); /* later END */

    /* Start is always read live, including bulk register seeds. No program
     * mutation is required to select another metadata result. */
    rsx_dispatch_method(&owner, 0x1EA0u, 1);
    assert(!owner.vp_metadata_dirty);
    check(); assert(memo.start == 1 && memo.info.instructions == 2);
    assert(check() == 2);
    {
        static u32 registers[RSX_DSP_NUM_REGS];
        memcpy(registers, owner.regs, sizeof(registers));
        registers[0x1EA0u >> 2] = 2;
        rsx_dispatch_seed_registers(&owner, registers, RSX_DSP_NUM_REGS);
        check(); assert(memo.start == 2 && memo.info.instructions == 1);
    }
    rsx_dispatch_method(&owner, 0x1EA0u, RSX_DSP_VP_INSTR);
    check(); assert(!memo.info.instructions);
    rsx_dispatch_method(&owner, 0x1EA0u, 0); check();

    /* Burst and generic upload remain whole-struct equivalent both when
     * content is unchanged and when a single instruction word changes. */
    for (u32 changed = 0; changed < 2; ++changed) {
        if (changed) words[5] = 25u << 22, words[6] = 3u << 8;
        rsx_dispatch_method(&owner, 0x1E9Cu, 0);
        memcpy(&generic, &owner, sizeof(owner));
        for (u32 i = 0; i < 32; ++i)
            rsx_dispatch_method(&generic, 0x0B80u + i * 4u, words[i]);
        assert(rsx_vp_burst_dispatch_32(&owner, 0, 0x0B80u, words));
        assert(memcmp(&generic, &owner, sizeof(owner)) == 0);
        assert(owner.vp_metadata_dirty == changed);
        check(); assert(memo.info.txl_mask == (changed ? 8u : 4u));
        assert(check() == 2);
    }
    /* No-room writes do not change the mirror or invalidate derived data. */
    rsx_dispatch_method(&owner, 0x1E9Cu, RSX_DSP_VP_INSTR);
    rsx_dispatch_method(&owner, 0x0B80u, 0xDEADBEEFu);
    assert(!owner.vp_metadata_dirty && check() == 2);

    /* Constants and unrelated render/FP/texture state remain live and do
     * not invalidate VP metadata. */
    {
        u32 constants[4] = {1, 2, 3, 4};
        rsx_dispatch_seed_transform_constants(&owner, constants, 4);
        rsx_dispatch_method(&owner, 0x0300u, 0x12345678u);
        rsx_dispatch_method(&owner, 0x08E4u, 0xAABBCCDDu);
        rsx_dispatch_method(&owner, 0x1A00u, 0xFFEEDDCCu);
        assert(!owner.vp_metadata_dirty && check() == 2);
        assert(owner.constants[0][0] == 1 && owner.regs[0x0300u >> 2] == 0x12345678u);
        assert(owner.regs[0x08E4u >> 2] == 0xAABBCCDDu && owner.regs[0x1A00u >> 2] == 0xFFEEDDCCu);
    }

    /* Reset invalidates an old memo even if its start still matches. Later
     * seed/upload recovers normally, including content-cache verified hits. */
    rsx_dispatch_init(&owner, NULL);
    assert(owner.vp_metadata_dirty);
    assert(check() == 0 && !memo.info.instructions);
    rsx_dispatch_seed_transform_program(&owner, words, 32);
    assert(owner.vp_metadata_dirty);
    assert(check() == 1 && memo.info.instructions == 3);
    assert(check() == 2);
    puts("PASS VP mirror metadata: miss output, partial/identical writes, END/start, seed/burst/reset, live state");
    return 0;
}
