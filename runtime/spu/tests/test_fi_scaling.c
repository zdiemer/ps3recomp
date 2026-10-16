/* Compare FI against an independent fixed-point oracle, including all
 * exponent/step fields, sign/base boundaries and host FP environment modes.
 * Define SPU_FI_BENCHMARK for a pinned local-cost comparison instead. */
#include "spu_helpers.h"
#include <assert.h>
#include <fenv.h>
#include <immintrin.h>
#include <stdio.h>
#ifdef SPU_FI_BENCHMARK
#include <windows.h>
#endif

static __attribute__((noinline)) u128 reference_fi(u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 4; ++i) {
        const uint32_t estimate = b._u32[i];
        int exponent = (estimate >> 23) & 255;
        if (!exponent) { r._u32[i] = 0; continue; }
        /* Independently evaluate (1.base - step * fraction) in Q32.
         * FI truncates, flushes exponent-zero operands/results, and treats
         * exponent 255 as extended range, regardless of the host FP mode. */
        uint64_t magnitude = ((uint64_t)(8192 + ((estimate >> 10) & 8191)) << 19)
                           - (uint64_t)(estimate & 1023) * (a._u32[i] & 524287);
        while (magnitude < (1ull << 32)) { magnitude <<= 1; --exponent; }
        r._u32[i] = exponent <= 0 ? 0 : (estimate & 0x80000000u)
                    | ((uint32_t)exponent << 23) | ((uint32_t)(magnitude >> 9) & 0x7fffffu);
    }
    return r;
}
static __attribute__((noinline)) u128 optimized_fi(u128 a, u128 b)
{ return spu_fi(a,b); }

static uint32_t rng = 0x51F1C100;
static uint32_t random_bits(void)
{ rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng; }

#ifndef SPU_FI_BENCHMARK
static unsigned long long comparisons;
static void compare(u128 a, u128 b)
{
    u128 expected = reference_fi(a,b), actual = optimized_fi(a,b);
    for (int lane=0;lane<4;++lane) {
        if (actual._u32[lane] != expected._u32[lane]) {
            fprintf(stderr,"FI mismatch a=%08X b=%08X expected=%08X actual=%08X csr=%08X\n",
                a._u32[lane],b._u32[lane],expected._u32[lane],actual._u32[lane],_mm_getcsr());
            assert(0);
        }
    }
    comparisons += 4;
}
int main(void)
{
    const int saved_round = fegetround();
    const unsigned saved_csr = _mm_getcsr();
    const int rounds[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
    const unsigned denormals[] = { 0, 0x40, 0x8000, 0x8040 };
    const uint32_t bases[] = { 0, 0x400, 0x3FFC00, 0x7FFC00 };
    const uint32_t fractions[] = { 0, 1, 0x3FFFF, 0x7FFFF };
    for (unsigned mode=0;mode<4;++mode) for(unsigned d=0;d<4;++d) {
        assert(fesetround(rounds[mode])==0);
        _mm_setcsr((_mm_getcsr() & ~0x8040u) | denormals[d]);
        for (unsigned exponent=0;exponent<256;++exponent)
            for(unsigned step=0;step<1024;++step)
                for(unsigned pattern=0;pattern<4;++pattern) {
                    u128 a,b;
                    for(unsigned lane=0;lane<4;++lane) {
                        a._u32[lane]=fractions[(lane+pattern)&3];
                        b._u32[lane]=(exponent<<23) | step | bases[lane] |
                                      (((lane+pattern)&1u)<<31);
                    }
                    compare(a,b);
                }
        /* Every interpolation numerator, spanning both signs and endpoint
         * step fields, independent of the estimate lookup tables. */
        for(unsigned fraction=0;fraction<0x80000;++fraction) {
            u128 a = spu_splat_u32(fraction), b;
            b._u32[0]=0x3F800000; b._u32[1]=0x3F8003FF;
            b._u32[2]=0xBF800001; b._u32[3]=0xBF8003FF;
            compare(a,b);
        }
        for(unsigned n=0;n<65536;++n) {
            u128 a,b;
            for(unsigned lane=0;lane<4;++lane) {
                a._u32[lane]=random_bits(); b._u32[lane]=random_bits();
            }
            compare(a,b);
        }
    }
    assert(fesetround(saved_round)==0); _mm_setcsr(saved_csr);
    printf("FI scaling: %llu exact lane comparisons across rounding and FTZ/DAZ modes passed\n",comparisons);
    return 0;
}
#else
static u128 inputs_a[4096],inputs_b[4096];
static volatile uint64_t consumed;
typedef u128 (*fi_fn)(u128,u128);
static __attribute__((noinline)) void measure(fi_fn fn, const char* mode)
{
    const unsigned count = 1000000;
    ULONGLONG begin,end; LARGE_INTEGER start,finish,frequency;
    uint64_t sum=0;
    assert(QueryPerformanceFrequency(&frequency));
    assert(QueryThreadCycleTime(GetCurrentThread(),&begin));
    assert(QueryPerformanceCounter(&start));
    for(unsigned n=0;n<count;++n) {
        u128 r=fn(inputs_a[n&4095],inputs_b[n&4095]);
        sum+=r._u32[0]; sum+=r._u32[1]; sum+=r._u32[2]; sum+=r._u32[3];
    }
    assert(QueryPerformanceCounter(&finish));
    assert(QueryThreadCycleTime(GetCurrentThread(),&end)); consumed=sum;
    printf("{\"mode\":\"%s\",\"calls\":%u,\"cpu\":%lu,\"cycles_per_call\":%.3f,\"elapsed_ms\":%.3f,\"checksum\":%llu}\n",
        mode,count,GetCurrentProcessorNumber(),(double)(end-begin)/count,
        (double)(finish.QuadPart-start.QuadPart)*1000/frequency.QuadPart,sum);
}
int main(void)
{
    DWORD_PTR allowed,system;
    assert(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system) && allowed);
    DWORD_PTR previous=SetThreadAffinityMask(GetCurrentThread(),allowed & (~allowed+1)); assert(previous);
    for(unsigned n=0;n<4096;++n) for(unsigned lane=0;lane<4;++lane) {
        inputs_a[n]._u32[lane]=random_bits();
        /* Typical finite normal estimate exponents; denormals are covered by
         * the exhaustive oracle rather than mixed into this local-cost run. */
        inputs_b[n]._u32[lane]=((80+random_bits()%101)<<23) | (random_bits()&0x807FFFFFu);
    }
    const unsigned order[]={0,1,1,0,1,0,0,1};
    for(unsigned n=0;n<8;++n)
        measure(order[n]?optimized_fi:reference_fi,order[n]?"optimized":"baseline");
    assert(SetThreadAffinityMask(GetCurrentThread(),previous));
    return 0;
}
#endif
