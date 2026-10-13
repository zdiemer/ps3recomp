#ifndef SPU_FLOAT_NATIVE_H
#define SPU_FLOAT_NATIVE_H
#include "../../include/ps3emu/ps3types.h"
#ifdef __cplusplus
extern "C" {
#endif
/* AVX-512 is confined to target-specific functions. Other compiled code keeps
 * its normal ISA; unsupported CPUs/OS vector state retain scalar arithmetic. */
#if defined(SPU_FLOAT_NATIVE_RUNTIME) && defined(_WIN32) && defined(__clang__) && defined(_M_X64)
#define SPU_FLOAT_NATIVE_HOST 1
extern int g_spu_float_native;
static inline int spu_float_native_active(void) {
    return __atomic_load_n(&g_spu_float_native,__ATOMIC_ACQUIRE)==1;
}
#else
static inline int spu_float_native_active(void) { return 0; }
#endif
int spu_float_native_available(void);
int spu_float_native_set_mode(int requested);
#ifdef SPU_FLOAT_NATIVE_HOST
#include <xmmintrin.h>
/* Keep the four lanes in vector registers across the Windows ABI boundary.
 * Returning/passing u128 unions through an out-of-line C call spills every
 * operand to memory, which is costly in the realtime audio DSP loops. */
__m128 __vectorcall spu_float_native_add_ps(__m128 a,__m128 b);
__m128 __vectorcall spu_float_native_mul_ps(__m128 a,__m128 b);
__m128 __vectorcall spu_float_native_madd_ps(__m128 a,__m128 b,__m128 c);
static inline u128 spu_float_native_add(u128 a,u128 b) {
    u128 r; _mm_storeu_ps(r._f32,spu_float_native_add_ps(_mm_loadu_ps(a._f32),_mm_loadu_ps(b._f32))); return r;
}
static inline u128 spu_float_native_mul(u128 a,u128 b) {
    u128 r; _mm_storeu_ps(r._f32,spu_float_native_mul_ps(_mm_loadu_ps(a._f32),_mm_loadu_ps(b._f32))); return r;
}
static inline u128 spu_float_native_madd(u128 a,u128 b,u128 c) {
    u128 r; _mm_storeu_ps(r._f32,spu_float_native_madd_ps(_mm_loadu_ps(a._f32),_mm_loadu_ps(b._f32),_mm_loadu_ps(c._f32))); return r;
}
#else
u128 spu_float_native_add(u128 a,u128 b);
u128 spu_float_native_mul(u128 a,u128 b);
u128 spu_float_native_madd(u128 a,u128 b,u128 c);
#endif
#ifdef __cplusplus
}
#endif
#endif
