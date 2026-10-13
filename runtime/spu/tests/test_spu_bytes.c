/* Independent SPU byte-position oracle for all rotate/zero-fill shift counts,
 * including negative immediates, ignored high bits and both profile paths. */
#define SPU_COMPARE_BYTE_OPS 1
#include "../spu_helpers.h"
#include <assert.h>
SPU_THREAD_LOCAL int g_spu_byte_ops_simd=1;
static uint32_t seed=0x12345678;
static uint32_t random_word(void) { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; }
static u128 oracle(u128 a,unsigned count,int kind) {
    u128 result={{0}};
    unsigned shift=count & (kind==0 ? 15 : 31);
    for (unsigned out=0;out<16;out++) {
        int source=kind==2 ? (int)out-(int)shift : (int)out+(int)shift;
        if (kind==0) source &= 15;
        if (source<0 || source>=16) continue;
        uint32_t byte=(a._u32[source/4]>>(24-(source%4)*8)) & 255;
        result._u32[out/4] |= byte<<(24-(out%4)*8);
    }
    return result;
}
static unsigned checks;
static void check(u128 got,u128 expected) {
    assert(!memcmp(&got,&expected,16)); ++checks;
}
int main(void) {
    const uint32_t high[]={0,0x80000000,0xFFFFFF00,0xA5A5B000};
    for (unsigned n=0;n<128;n++) {
        u128 a,b;
        for (unsigned lane=0;lane<4;lane++) {
            a._u32[lane]=n==0 ? 0x00010203+lane*0x04040404 : n==1 ? 0 : n==2 ? ~0u : random_word();
            b._u32[lane]=random_word();
        }
        for (unsigned mode=0;mode<2;mode++) {
            g_spu_byte_ops_simd=mode;
            for (unsigned c=0;c<256;c++) for (unsigned h=0;h<4;h++) {
                uint32_t count=high[h]|c; b._u32[0]=count;
                int immediate=(int)c-128;
                check(spu_shlqbyi(a,immediate),oracle(a,(unsigned)immediate,1));
                check(spu_rotqbyi(a,immediate),oracle(a,(unsigned)immediate,0));
                check(spu_rotqmbyi(a,immediate),oracle(a,0u-(unsigned)immediate,2));
                check(spu_shlqby(a,b),oracle(a,count,1));
                check(spu_rotqby(a,b),oracle(a,count,0));
                check(spu_shlqbybi(a,b),oracle(a,count>>3,1));
                check(spu_rotqbybi(a,b),oracle(a,count>>3,0));
                check(spu_rotqmby(a,b),oracle(a,0u-count,2));
                check(spu_rotqmbybi(a,b),oracle(a,0u-(count>>3),2));
            }
        }
    }
    printf("SPU byte operations: %u scalar/SIMD cases match the independent byte-position oracle\n",checks);
    return 0;
}
