/* Optional process-lifetime Win32 coherence-lock contention diagnostic. */
#ifndef SPU_COHERENCY_PROFILE_H
#define SPU_COHERENCY_PROFILE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef SPU_COH_LOCK_PROFILE_CAPACITY
#define SPU_COH_LOCK_PROFILE_CAPACITY 262144u
#endif
#define SPU_COH_LOCK_PROFILE_THRESHOLD_US 1000u
typedef struct spu_coh_lock_profile_row {
    uint64_t sequence, begin_qpc, end_qpc, caller;
    uint32_t host_tid, reserved;
} spu_coh_lock_profile_row;
typedef struct spu_coh_lock_profile_snapshot {
    const spu_coh_lock_profile_row* rows;
    uint64_t frequency, begin_qpc, end_qpc, image_base, threshold_ticks;
    uint64_t completed_contentions, below_threshold, invalid_clocks, recorded;
    uint64_t max_overwritten_end_qpc, inflight_at_stop, admitted_contentions;
    uint32_t started, clock_valid, admission_overflow;
} spu_coh_lock_profile_snapshot;
/* Call before guest workers, without holding the lock. One capture per process;
 * repeated start while active is harmless, restart after stop is rejected.
 * No SDK environment option: the caller explicitly enables this diagnostic. */
int spu_coh_lock_profile_start(void);
/* Freezes under the actual lock; no writer drain or thread termination. Admitted
 * unfinished acquisitions are counted as inflight_at_stop, then discarded on
 * completion. Frozen rows/metadata never change, including on repeated stop.
 * Stop and dump must not be called while holding the nonrecursive lock. */
void spu_coh_lock_profile_stop(spu_coh_lock_profile_snapshot* snapshot);
/* Stops if necessary, writes CSV + <path>.meta.json outside the actual lock.
 * Returns 0 for optional export failure. Serialize export calls externally;
 * repeated exports contain the same frozen capture. Non-Windows is a no-op. */
int spu_coh_lock_profile_dump(const char* path);
#ifdef __cplusplus
}
#endif
#endif
