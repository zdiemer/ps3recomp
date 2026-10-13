#include "../spu_float_native.c"
#include <xmmintrin.h>
#include <windows.h>
#include <assert.h>
#include <stdio.h>
static uint32_t seed=0x631df028;
static uint32_t random_word(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static unsigned long long checks;
static void verify(u128 a,u128 b,u128 c) {
    const unsigned before=_mm_getcsr();
    u128 actual[9]={spu_float_native_add(a,b),spu_float_native_mul(a,b),spu_float_native_madd(a,b,c),
        spu_fa(a,b),spu_fs(a,b),spu_fm(a,b),spu_fma(a,b,c),spu_fms(a,b,c),spu_fnms(a,b,c)};
    assert(_mm_getcsr()==before);
    for (unsigned i=0;i<4;++i) {
        const uint32_t av=a._u32[i],bv=b._u32[i],cv=c._u32[i];
        uint32_t want[]={spu_float_add(av,bv),spu_float_mul(av,bv),spu_float_madd(av,bv,cv),
            spu_float_add(av,bv),spu_float_add(av,bv^0x80000000u),spu_float_mul(av,bv),
            spu_float_madd(av,bv,cv),spu_float_madd(av,bv,cv^0x80000000u),spu_float_madd(av^0x80000000u,bv,cv)};
        for(unsigned op=0;op<9;++op) {
            if (want[op]!=actual[op]._u32[i]) {
                fprintf(stderr,"FAIL op=%u a=%08x b=%08x c=%08x expected=%08x actual=%08x mxcsr=%x\n",op,a._u32[i],b._u32[i],c._u32[i],want[op],actual[op]._u32[i],before);
                abort();
            }
            ++checks;
        }
    }
}
static uint64_t cycles(void) { ULONG64 n; assert(QueryThreadCycleTime(GetCurrentThread(),&n));return n; }
static volatile uint32_t sink;
int main(void) {
    printf("AVX512F CPU+OS available=%d\n",spu_float_native_available()); if (!spu_float_native_available()) {
        assert(!spu_float_native_set_mode(1) && !spu_float_native_active());
        puts("Native float: unsupported CPU/OS retains scalar fallback; hardware oracle skipped");return 0;
    }
    assert(spu_float_native_set_mode(1) && spu_float_native_active());
    const uint32_t edge[]={0,1,0x007fffff,0x00800000,0x00800001,0x3f7fffff,0x3f800000,0x3f800001,0x7effffff,0x7f000000,0x7f7fffff,0x7f800000,0x7fffffff,0x80000000,0x80000001,0xbf800000,0xff800000,0xffffffff};
    const unsigned saved=_mm_getcsr();
    for (unsigned mode=0;mode<8;++mode) {
        _mm_setcsr((saved&~(0x6000u|0x8040u))|((mode&3)<<13)|(mode&4?0x8040u:0));
        for (unsigned i=0;i<18;++i) for (unsigned j=0;j<18;++j) for(unsigned k=0;k<18;++k) {
            u128 a,b,c; for(unsigned l=0;l<4;++l) { a._u32[l]=edge[i];b._u32[l]=edge[j];c._u32[l]=edge[k]; } verify(a,b,c);
        }
        for (unsigned i=0;i<250000;++i) {
            u128 a,b,c;for(unsigned l=0;l<4;++l) { a._u32[l]=random_word();b._u32[l]=random_word();c._u32[l]=random_word(); }
            verify(a,b,c);
        }
    }
    assert(!spu_float_native_set_mode(0));
    u128 zero={0};verify(zero,zero,zero);
    assert(spu_float_native_set_mode(1));
    _mm_setcsr(saved); printf("%llu independent hardware/reference lane comparisons passed\n",checks);
    u128 a,b,c;for (unsigned l=0;l<4;++l) { a._u32[l]=0x3f800000+l*173;b._u32[l]=0x3c23d70a+l*37;c._u32[l]=0x3f000000+l*17; }
    u128 factors[256],offsets[256];
    for(unsigned n=0;n<256;++n) for(unsigned lane=0;lane<4;++lane) {
        factors[n]._u32[lane]=0x3d000000|(random_word()&0x7fffff);
        offsets[n]._u32[lane]=0x3f000000|(random_word()&0x7fffff);
    }
    for (unsigned run=0;run<4;++run) for(unsigned mode=0;mode<2;++mode) {
        const unsigned repeats=2000000;uint64_t begin=cycles();
        for(unsigned i=0;i<repeats;++i) {
            if (mode) a=spu_float_native_madd(a,factors[i&255],offsets[i&255]); else for(unsigned l=0;l<4;++l) a._u32[l]=spu_float_madd(a._u32[l],factors[i&255]._u32[l],offsets[i&255]._u32[l]);
        }
        sink=a._u32[0]^a._u32[1]^a._u32[2]^a._u32[3]; printf("run=%u native=%u cycles/vector=%.3f\n",run,mode,(double)(cycles()-begin)/repeats);
    }
    // Deliberate corruption must publish the scalar result and latch failure.
#ifdef SPU_FLOAT_NATIVE_HOST
    validate=1; u128 bad={0};bad._u32[0]=1;
    u128 corrected=validate_result(bad,zero,zero,zero,0);
    assert(!memcmp(&corrected,&zero,sizeof(zero)) && failures==1 && audits==1);
    assert(!spu_float_native_active() && !spu_float_native_set_mode(1));
    puts("Native float audit: corruption replaced with scalar output; failure cannot be re-enabled");
#endif
}
