#ifndef PS3RECOMP_ENV_ONCE_H
#define PS3RECOMP_ENV_ONCE_H
#include <stdlib.h>

/* getenv once per call site. The CRT's getenv takes the environment lock and
 * scans the whole block; SPU job threads called it per job, per DMA engine
 * init and per dispatch, contending for that lock across every worker. The
 * environment is fixed before guest code runs, so the first read stands.
 *
 *   static ps3_env_once_slot s_trace;
 *   if (ps3_env_once(&s_trace, "SPU_TRACE")) ...
 */
typedef struct { const char* value; int state; } ps3_env_once_slot;

static inline const char* ps3_env_once(ps3_env_once_slot* slot, const char* name)
{
    if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE))
        return __atomic_load_n(&slot->value, __ATOMIC_RELAXED);
    const char* value = getenv(name);
    __atomic_store_n(&slot->value, value, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->state, 1, __ATOMIC_RELEASE);
    return value;
}
#endif
