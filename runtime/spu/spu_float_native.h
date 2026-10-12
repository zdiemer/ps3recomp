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
void spu_float_native_update(void);
void spu_float_native_summary(void);
u128 spu_float_native_add(u128 a,u128 b);
u128 spu_float_native_mul(u128 a,u128 b);
u128 spu_float_native_madd(u128 a,u128 b,u128 c);
#ifdef __cplusplus
}
#endif
#endif
