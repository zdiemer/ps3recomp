/* Opt-in native arithmetic for the measured SPU float hotspot.
 * Ordinary lanes use hardware round-toward-zero with suppressed exceptions;
 * extended-range/overflow-risk lanes use the corrected integer arithmetic. */
#include "spu_helpers.h"
#include "spu_float_native.h"
#include <stdlib.h>
#include <stdio.h>

#ifdef SPU_FLOAT_NATIVE_HOST
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
int g_spu_float_native;
static INIT_ONCE once=INIT_ONCE_STATIC_INIT;
static int supported,validate,failed,reported=-1;
static const char* control;
static ULONGLONG next_check;
static unsigned long long audits,failures;
__attribute__((target("xsave"))) static unsigned long long os_vector_state(void) { return _xgetbv(0); }
static BOOL CALLBACK initialize(PINIT_ONCE unused,PVOID param,PVOID* context) {
    (void)unused;(void)param;(void)context;
    int info[4]; __cpuidex(info,1,0);
    if ((info[2]&(1<<27)) && (os_vector_state()&0xe6)==0xe6) {
        __cpuidex(info,7,0); supported=(info[1]&(1<<16))!=0;
    }
    control=getenv("SPU_FLOAT_NATIVE_FILE");
    const char* option=getenv("SPU_FLOAT_NATIVE_VALIDATE");
    validate=option && *option=='1';
    return TRUE;
}
int spu_float_native_available(void) {
    InitOnceExecuteOnce(&once,initialize,NULL,NULL); return supported;
}
int spu_float_native_set_mode(int requested) {
    int enabled=spu_float_native_available() && requested;
    int current=__atomic_load_n(&g_spu_float_native,__ATOMIC_RELAXED);
    while (current>=0 && !__atomic_compare_exchange_n(&g_spu_float_native,&current,enabled,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED)) {}
    if (current<0) enabled=0;
    if (reported!=enabled) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        fprintf(stderr,"[spu-float-native-mode] enabled=%d qpc=%lld\n",enabled,now.QuadPart);
        reported=enabled;
    }
    return enabled;
}
void spu_float_native_update(void) {
    static int initialized;
    if (!initialized) {
        initialized=1;
        const char* option=getenv("SPU_FLOAT_NATIVE");
        if (control || option) spu_float_native_set_mode(option && *option=='1');
        else spu_float_native_available();
        /* control is initialized by available(), including when no mode env exists. */
        if (control && reported<0) spu_float_native_set_mode(0);
    }
    ULONGLONG now=GetTickCount64();
    if (control && now>=next_check) {
        next_check=now+100; FILE* file=fopen(control,"r"); unsigned enabled;
        if (file) {
            if (fscanf(file,"%u",&enabled)==1 && enabled<=1) spu_float_native_set_mode((int)enabled);
            fclose(file);
        }
    }
}
void spu_float_native_summary(void) {
    fprintf(stderr,"[spu-float-native-summary] supported=%d enabled=%d validate=%d audits=%llu failures=%llu failed=%d\n",
        supported,spu_float_native_active(),validate,__atomic_load_n(&audits,__ATOMIC_RELAXED),
        __atomic_load_n(&failures,__ATOMIC_RELAXED),__atomic_load_n(&failed,__ATOMIC_RELAXED));
}
static u128 validate_result(u128 result,u128 a,u128 b,u128 c,unsigned op) {
    if (!validate) return result;
    u128 expected;
    for (unsigned i=0;i<4;++i)
        expected._u32[i]=op==0 ? spu_float_add(a._u32[i],b._u32[i]) :
            op==1 ? spu_float_mul(a._u32[i],b._u32[i]) : spu_float_madd(a._u32[i],b._u32[i],c._u32[i]);
    __atomic_fetch_add(&audits,1,__ATOMIC_RELAXED);
    if (memcmp(&result,&expected,sizeof(result))) {
        __atomic_fetch_add(&failures,1,__ATOMIC_RELAXED);
        if (!__atomic_exchange_n(&failed,1,__ATOMIC_RELAXED))
            fprintf(stderr,"[spu-float-native] output mismatch; disabling native arithmetic\n");
        __atomic_store_n(&g_spu_float_native,-1,__ATOMIC_RELEASE);
    }
    return expected;
}
/* RelWithDebInfo uses /Ob1: these small helpers otherwise become calls across
 * the target-specific kernel boundary, spilling vectors and vzeroupper-ing. */
static inline __attribute__((always_inline)) __m128i canonical(__m128i x) {
    __m128i exponent=_mm_and_si128(x,_mm_set1_epi32(0x7f800000));
    return _mm_andnot_si128(_mm_cmpeq_epi32(exponent,_mm_setzero_si128()),x);
}
static inline __attribute__((always_inline)) __m128i flush(__m128i x) { return canonical(x); }
#define NATIVE __attribute__((target("avx512f")))
#define ROUND (_MM_FROUND_TO_ZERO|_MM_FROUND_NO_EXC)
NATIVE __m128 __vectorcall spu_float_native_add_ps(__m128 av,__m128 bv) {
    u128 a,b; _mm_storeu_ps(a._f32,av); _mm_storeu_ps(b._f32,bv);
    __m128i aa=canonical(_mm_loadu_si128((const __m128i*)&a)),bb=canonical(_mm_loadu_si128((const __m128i*)&b));
    const __m128i em=_mm_set1_epi32(0x7f800000),limit=_mm_set1_epi32(0x7e800000);
    const int slow=_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(
        _mm_cmpgt_epi32(_mm_and_si128(aa,em),limit),
        _mm_cmpgt_epi32(_mm_and_si128(bb,em),limit))));
    __m512 result=_mm512_maskz_add_round_ps(15,_mm512_castps128_ps512(_mm_castsi128_ps(aa)),
        _mm512_castps128_ps512(_mm_castsi128_ps(bb)),ROUND);
    u128 r; _mm_storeu_si128((__m128i*)&r,flush(_mm_castps_si128(_mm512_castps512_ps128(result))));
    for (int i=0;i<4;++i) if (slow&(1<<i)) r._u32[i]=spu_float_add(a._u32[i],b._u32[i]);
    if (validate) r=validate_result(r,a,b,(u128){0},0);
    return _mm_loadu_ps(r._f32);
}
NATIVE __m128 __vectorcall spu_float_native_mul_ps(__m128 av,__m128 bv) {
    u128 a,b; _mm_storeu_ps(a._f32,av); _mm_storeu_ps(b._f32,bv);
    __m128i aa=canonical(_mm_loadu_si128((const __m128i*)&a)),bb=canonical(_mm_loadu_si128((const __m128i*)&b));
    const __m128i em=_mm_set1_epi32(0xff);
    __m128i ea=_mm_and_si128(_mm_srli_epi32(aa,23),em);
    __m128i eb=_mm_and_si128(_mm_srli_epi32(bb,23),em);
    __m128i unsafe=_mm_or_si128(_mm_cmpeq_epi32(ea,em),_mm_cmpeq_epi32(eb,em));
    unsafe=_mm_or_si128(unsafe,_mm_cmpgt_epi32(_mm_add_epi32(ea,eb),_mm_set1_epi32(379)));
    int slow=_mm_movemask_ps(_mm_castsi128_ps(unsafe));
    __m512 result=_mm512_maskz_mul_round_ps(15,_mm512_castps128_ps512(_mm_castsi128_ps(aa)),
        _mm512_castps128_ps512(_mm_castsi128_ps(bb)),ROUND);
    u128 r; _mm_storeu_si128((__m128i*)&r,flush(_mm_castps_si128(_mm512_castps512_ps128(result))));
    for (int i=0;i<4;++i) if (slow&(1<<i)) r._u32[i]=spu_float_mul(a._u32[i],b._u32[i]);
    if (validate) r=validate_result(r,a,b,(u128){0},1);
    return _mm_loadu_ps(r._f32);
}
NATIVE __m128 __vectorcall spu_float_native_madd_ps(__m128 av,__m128 bv,__m128 cv) {
    u128 a,b,c; _mm_storeu_ps(a._f32,av); _mm_storeu_ps(b._f32,bv); _mm_storeu_ps(c._f32,cv);
    __m128i aa=canonical(_mm_loadu_si128((const __m128i*)&a)),bb=canonical(_mm_loadu_si128((const __m128i*)&b)),cc=canonical(_mm_loadu_si128((const __m128i*)&c));
    const __m128i em=_mm_set1_epi32(0xff);
    __m128i ea=_mm_and_si128(_mm_srli_epi32(aa,23),em);
    __m128i eb=_mm_and_si128(_mm_srli_epi32(bb,23),em);
    __m128i ec=_mm_and_si128(_mm_srli_epi32(cc,23),em);
    __m128i unsafe=_mm_or_si128(_mm_cmpeq_epi32(ea,em),_mm_cmpeq_epi32(eb,em));
    unsafe=_mm_or_si128(unsafe,_mm_cmpgt_epi32(ec,_mm_set1_epi32(253)));
    unsafe=_mm_or_si128(unsafe,_mm_cmpgt_epi32(_mm_add_epi32(ea,eb),_mm_set1_epi32(379)));
    int slow=_mm_movemask_ps(_mm_castsi128_ps(unsafe));
    __m512 result=_mm512_maskz_fmadd_round_ps(15,_mm512_castps128_ps512(_mm_castsi128_ps(aa)),
        _mm512_castps128_ps512(_mm_castsi128_ps(bb)),_mm512_castps128_ps512(_mm_castsi128_ps(cc)),ROUND);
    u128 r; _mm_storeu_si128((__m128i*)&r,flush(_mm_castps_si128(_mm512_castps512_ps128(result))));
    for (int i=0;i<4;++i) if (slow&(1<<i)) r._u32[i]=spu_float_madd(a._u32[i],b._u32[i],c._u32[i]);
    if (validate) r=validate_result(r,a,b,c,2);
    return _mm_loadu_ps(r._f32);
}

#else
int spu_float_native_available(void) { return 0; }
int spu_float_native_set_mode(int requested) { (void)requested; return 0; }
void spu_float_native_update(void) {}
void spu_float_native_summary(void) {}
u128 spu_float_native_add(u128 a,u128 b) { u128 r; for(int i=0;i<4;++i) r._u32[i]=spu_float_add(a._u32[i],b._u32[i]); return r; }
u128 spu_float_native_mul(u128 a,u128 b) { u128 r; for(int i=0;i<4;++i) r._u32[i]=spu_float_mul(a._u32[i],b._u32[i]); return r; }
u128 spu_float_native_madd(u128 a,u128 b,u128 c) { u128 r; for(int i=0;i<4;++i) r._u32[i]=spu_float_madd(a._u32[i],b._u32[i],c._u32[i]); return r; }
#endif
