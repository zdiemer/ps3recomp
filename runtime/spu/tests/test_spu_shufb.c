/* spu_shufb (pshufb / NEON tbl path) must match the byte-loop reference for
 * every selector class: a/b sources, 0x00 / 0xFF / 0x80 specials. Build with
 * -msse4.1 on x86 so the SIMD version is the one under test (NEON is always
 * on for arm64). */
#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
#if !defined(__SSE4_1__) && !defined(__ARM_NEON)
    puts("test_spu_shufb: built without SSE4.1 or NEON, nothing to compare");
    return 0;
#else
    /* Exhaust every selector at every output position with distinct source
     * bytes. This catches host/SPU byte-order mistakes and selector bit 5/6
     * aliases independently of random coverage. Neighbors retain a mixed
     * control pattern, exercising the cross-byte blend-mask shift. */
    for (unsigned selector=0; selector<256; ++selector) {
        for (unsigned position=0; position<16; ++position) {
            u128 a,b,c;
            for (unsigned byte=0; byte<16; ++byte) {
                a._u8[SPU_W(byte)]=(uint8_t)(byte+1);
                b._u8[SPU_W(byte)]=(uint8_t)(byte+65);
                c._u8[SPU_W(byte)]=(uint8_t)(byte*17);
            }
            c._u8[SPU_W(position)]=(uint8_t)selector;
            u128 x=spu_shufb(a,b,c),y=spu_shufb_ref(a,b,c);
            if(memcmp(&x,&y,16)) {
                printf("test_spu_shufb: selector %u position %u mismatch\n",selector,position);
                return 1;
            }
        }
    }
    srand(12345);
    for (int it = 0; it < 200000; it++) {
        u128 a, b, c;
        for (int i = 0; i < 16; i++) {
            a._u8[i] = (uint8_t)rand(); b._u8[i] = (uint8_t)rand();
            c._u8[i] = (uint8_t)rand();          /* covers all 256 selector values */
        }
        u128 x = spu_shufb(a, b, c), y = spu_shufb_ref(a, b, c);
        if (memcmp(&x, &y, 16)) {
            printf("test_spu_shufb: MISMATCH at iteration %d\n", it);
            return 1;
        }
    }
    puts("test_spu_shufb: ok (4096 exhaustive selector/position + 200000 random cases)");
    return 0;
#endif
}
