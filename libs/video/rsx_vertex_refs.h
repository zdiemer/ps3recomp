/* Native batch decoding of contiguous big-endian index references.
 * Caller validates the source span and reserves count output records. Source
 * and output must not overlap. A restart marker returns false; output may be
 * partially written, so the caller must discard it and retain its cut builder.
 * No reads/writes extend beyond count, including unaligned and short tails. */
#ifndef PS3RECOMP_RSX_VERTEX_REFS_H
#define PS3RECOMP_RSX_VERTEX_REFS_H
#include <stddef.h>
#include "rsx_vertex_compact.h"
#if !defined(RSX_REFS_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
#include <emmintrin.h>
#define RSX_REFS_SSE2 1
#endif

static int rsx_vertex_refs_decode_be(const u8* source, u32 count, int wide,
    u32 base, int restart, u32 restart_value, rsx_vertex_ref* output)
{
    u32 i = 0;
#ifdef RSX_REFS_SSE2
    const __m128i bases = _mm_set1_epi32((int)base);
    if (wide) {
        const __m128i marker = _mm_set1_epi32((int)restart_value);
        for (; count - i >= 4; i += 4) {
            __m128i v = _mm_loadu_si128((const __m128i*)(source + (size_t)i * 4));
            v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
            v = _mm_shufflehi_epi16(_mm_shufflelo_epi16(v, 0xB1), 0xB1);
            if (restart && _mm_movemask_epi8(_mm_cmpeq_epi32(v, marker))) return 0;
            _mm_storeu_si128((__m128i*)(output + i), _mm_unpacklo_epi32(v, bases));
            _mm_storeu_si128((__m128i*)(output + i + 2), _mm_unpackhi_epi32(v, bases));
        }
    } else {
        const __m128i zero = _mm_setzero_si128();
        const __m128i marker = _mm_set1_epi16((short)restart_value);
        for (; count - i >= 8; i += 8) {
            __m128i v = _mm_loadu_si128((const __m128i*)(source + (size_t)i * 2));
            v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
            if (restart && restart_value <= 0xFFFFu &&
                _mm_movemask_epi8(_mm_cmpeq_epi16(v, marker))) return 0;
            const __m128i lo = _mm_unpacklo_epi16(v, zero);
            const __m128i hi = _mm_unpackhi_epi16(v, zero);
            _mm_storeu_si128((__m128i*)(output + i), _mm_unpacklo_epi32(lo, bases));
            _mm_storeu_si128((__m128i*)(output + i + 2), _mm_unpackhi_epi32(lo, bases));
            _mm_storeu_si128((__m128i*)(output + i + 4), _mm_unpacklo_epi32(hi, bases));
            _mm_storeu_si128((__m128i*)(output + i + 6), _mm_unpackhi_epi32(hi, bases));
        }
    }
#endif
    for (; i < count; ++i) {
        const u8* p = source + (size_t)i * (wide ? 4u : 2u);
        const u32 value = wide ? ((u32)p[0] << 24) | ((u32)p[1] << 16) |
            ((u32)p[2] << 8) | p[3] : ((u32)p[0] << 8) | p[1];
        if (restart && value == restart_value) return 0;
        output[i].vertex_id = value;
        output[i].base_index = base;
    }
    return 1;
}
#endif
