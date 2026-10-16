/* Bit-position oracle: independent of the implementation's word shifts.
 * Build with the same optimization/ISA flags as the lifted SPU code. */
#include "../spu_helpers.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static u128 reference(u128 a, unsigned shift, int direction, int rotate)
{
    u128 r = spu_zero();
    for (int bit = 0; bit < 128; ++bit) {
        int source = bit + direction * (int)shift;
        if (rotate) source &= 127;
        if (source >= 0 && source < 128) {
            unsigned value = (a._u32[source / 32] >> (31 - source % 32)) & 1u;
            r._u32[bit / 32] |= value << (31 - bit % 32);
        }
    }
    return r;
}

static void equal(u128 actual, u128 expected)
{
    assert(memcmp(&actual, &expected, sizeof(actual)) == 0);
}

static void check(u128 a, unsigned count)
{
    u128 b = spu_splat_u32(count);
    equal(spu_shlqbii(a, (int)count), reference(a, count & 7, 1, 0));
    equal(spu_shlqbi(a, b), reference(a, count & 7, 1, 0));
    equal(spu_rotqbii(a, (int)count), reference(a, count & 7, 1, 1));
    equal(spu_rotqbi(a, b), reference(a, count & 7, 1, 1));
    equal(spu_rotqmbii(a, (int)count), reference(a, (0u-count) & 7, -1, 0));
    equal(spu_rotqmbi(a, b), reference(a, (0u-count) & 7, -1, 0));
}

int main(void)
{
    unsigned cases = 0;
    for (int bit = 0; bit < 128; ++bit) {
        u128 a = spu_zero();
        a._u32[bit / 32] = 1u << (bit % 32);
        for (int count = -128; count < 128; ++count) {
            check(a, (unsigned)count); ++cases;
        }
    }
    uint32_t state = 0x18a12345u;
    for (unsigned i = 0; i < 10000; ++i) {
        u128 a;
        for (int lane = 0; lane < 4; ++lane) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            a._u32[lane] = state;
        }
        const unsigned counts[] = {state, 0, 1, 7, 8, 0x80000000u, UINT_MAX};
        for (unsigned j = 0; j < sizeof(counts)/sizeof(counts[0]); ++j) {
            check(a, counts[j]); ++cases;
        }
    }
    printf("PASS %u quadword vectors/counts, six operations each\n", cases);
    return 0;
}
