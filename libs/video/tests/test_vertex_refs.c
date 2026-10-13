#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "../rsx_vertex_refs.h"

static void encode(u8* p, u32 value, int wide)
{
    const unsigned bytes = wide ? 4u : 2u;
    for (unsigned j = 0; j < bytes; ++j)
        p[j] = (u8)(value >> ((bytes - j - 1u) * 8u));
}

static void check(u8* source, rsx_vertex_ref* output, u32 count, int wide,
    u32 base)
{
    const unsigned bytes = wide ? 4u : 2u;
    u32 values[65];
    for (u32 i = 0; i < count; ++i) {
        values[i] = (i * 0x9E3779B9u + 0x810203F4u) & (wide ? ~0u : 0xFFFFu);
        encode(source + i * bytes, values[i], wide);
    }
    assert(rsx_vertex_refs_decode_be(source, count, wide, base, 0, 0, output));
    for (u32 i = 0; i < count; ++i) {
        assert(output[i].vertex_id == values[i]);
        assert(output[i].base_index == base);
    }
    /* Every lane and tail must reject an actual restart marker. A failed
     * speculative batch is discarded by the caller, not partially published. */
    for (u32 i = 0; i < count; ++i)
        assert(!rsx_vertex_refs_decode_be(source, count, wide, base, 1, values[i], output));
    assert(rsx_vertex_refs_decode_be(source, count, wide, base, 1, ~0u, output));
    if (!wide) {
        /* A 32-bit restart value cannot be silently truncated to 16 bits. */
        assert(rsx_vertex_refs_decode_be(source, count, wide, base, 1,
            count ? values[count / 2] | 0x10000u : 0x10000u, output));
    }
    if (count) {
        encode(source, wide ? ~0u : 0xFFFFu, wide);
        assert(!rsx_vertex_refs_decode_be(source, count, wide, base, 1,
            wide ? ~0u : 0xFFFFu, output));
        assert(rsx_vertex_refs_decode_be(source, count, wide, base, 0, ~0u, output));
        assert(output[0].vertex_id == (wide ? ~0u : 0xFFFFu));
    }
}

int main(void)
{
    _Static_assert(sizeof(rsx_vertex_ref) == 8 && offsetof(rsx_vertex_ref, base_index) == 4,
        "SIMD stores require two contiguous 32-bit fields");
    assert(rsx_vertex_refs_decode_be(NULL, 0, 0, 0, 1, 0, NULL));
    unsigned cases = 0;
    u8 input[65 * 4 + 32], result[65 * 8 + 32];
    for (int wide = 0; wide <= 1; ++wide)
    for (unsigned alignment = 0; alignment < 16; ++alignment)
    for (u32 count = 0; count <= 65; ++count)
    for (unsigned output_offset = 0; output_offset < 16; output_offset += 4) {
        memset(result, 0xA5, sizeof(result));
        check(input + alignment, (rsx_vertex_ref*)(result + output_offset), count,
            wide, count & 1 ? 0xFEDCBA98u : 0);
        for (unsigned i = 0; i < output_offset; ++i) assert(result[i] == 0xA5);
        for (size_t i = output_offset + count * sizeof(rsx_vertex_ref); i < sizeof(result); ++i)
            assert(result[i] == 0xA5);
        ++cases;
    }
    SYSTEM_INFO info; GetSystemInfo(&info);
    u8* guarded_input = VirtualAlloc(NULL, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    u8* guarded_output = VirtualAlloc(NULL, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(guarded_input && guarded_output);
    DWORD previous;
    assert(VirtualProtect(guarded_input + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous));
    assert(VirtualProtect(guarded_output + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous));
    for (int wide = 0; wide <= 1; ++wide)
    for (u32 count = 0; count <= 65; ++count) {
        check(guarded_input + info.dwPageSize - count * (wide ? 4u : 2u),
            (rsx_vertex_ref*)(guarded_output + info.dwPageSize - count * sizeof(rsx_vertex_ref)),
            count, wide, ~0u);
        ++cases;
    }
    assert(VirtualFree(guarded_input, 0, MEM_RELEASE));
    assert(VirtualFree(guarded_output, 0, MEM_RELEASE));
    printf("Native index references: %u alignment/tail/restart/bounds cases passed\n", cases);
    return 0;
}
