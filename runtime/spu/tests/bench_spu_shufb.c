/* Standalone Windows CPU-cycle experiment. Legacy SIMD is retained only here
 * to compare against the current helper with identical dependent workloads.
 * Not a whole-game benchmark. Compile with -msse4.1, optimization and no NDEBUG. */
#include "../spu_helpers.h"
#include <assert.h>
#include <stdio.h>
#include <windows.h>

static inline u128 legacy_shufb(u128 a, u128 b, u128 c) {
    const __m128i sw = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12);
    __m128i A, B, C; memcpy(&A, &a, 16); memcpy(&B, &b, 16); memcpy(&C, &c, 16);
    A = _mm_shuffle_epi8(A, sw); B = _mm_shuffle_epi8(B, sw); C = _mm_shuffle_epi8(C, sw);
    const __m128i idx = _mm_and_si128(C, _mm_set1_epi8(0x0F));
    const __m128i fromb = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
    __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(A, idx), _mm_shuffle_epi8(B, idx), fromb);
    /* Special selectors: 10x -> 0x00, 110 -> 0xFF, 111 -> 0x80. */
    const __m128i m80 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8((char)0x80)), _mm_set1_epi8((char)0x80));
    const __m128i mC0 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8((char)0xC0)), _mm_set1_epi8((char)0xC0));
    const __m128i mE0 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8((char)0xE0)), _mm_set1_epi8((char)0xE0));
    const __m128i special = _mm_blendv_epi8(mC0, _mm_set1_epi8((char)0x80), mE0);
    r = _mm_shuffle_epi8(_mm_blendv_epi8(r, special, m80), sw);
    u128 out; memcpy(&out, &r, 16); return out;
}
static uint32_t seed=0x1234abcd;
static uint32_t next(void) { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; }
static u128 controls[256];
static volatile uint32_t sink;
__declspec(noinline) static u128 old_chain(u128 a,u128 b,unsigned n) {
    for(unsigned i=0;i<n;i++) { a=legacy_shufb(a,b,controls[i&255]); b=spu_xor(a,b); }
    return a;
}
__declspec(noinline) static u128 new_chain(u128 a,u128 b,unsigned n) {
    for(unsigned i=0;i<n;i++) { a=spu_shufb(a,b,controls[i&255]); b=spu_xor(a,b); }
    return a;
}
static uint64_t cycles(void) { ULONG64 c; assert(QueryThreadCycleTime(GetCurrentThread(),&c)); return c; }
int main(void) {
    unsigned checks=0;
    for(unsigned n=0;n<200000;n++) {
        u128 a,b,c;
        for(unsigned lane=0;lane<4;lane++) { a._u32[lane]=next(); b._u32[lane]=next(); c._u32[lane]=next(); }
        if(n<256) memset(&c,n,16);
        u128 x=spu_shufb(a,b,c),y=spu_shufb_ref(a,b,c);
        assert(!memcmp(&x,&y,16)); checks++;
    }
    printf("Direct shuffle oracle: %u cases passed\n",checks);
    for(unsigned i=0;i<256;i++) for(unsigned lane=0;lane<4;lane++) controls[i]._u32[lane]=next();
    u128 a=spu_il(123),b=spu_il(456);
    const unsigned n=20000000;
    for(unsigned repeat=0;repeat<6;repeat++) {
        uint64_t begin=cycles(); u128 old=old_chain(a,b,n); uint64_t oldcost=cycles()-begin;
        begin=cycles(); u128 optimized=new_chain(a,b,n); uint64_t newcost=cycles()-begin;
        sink=old._u32[0]^optimized._u32[0]; assert(!memcmp(&old,&optimized,16));
        printf("%u old %.3f cycles/op direct %.3f cycles/op ratio %.3f\n",repeat,(double)oldcost/n,(double)newcost/n,(double)oldcost/newcost);
    }
}
