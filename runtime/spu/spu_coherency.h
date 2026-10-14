/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * An SPU that issues GETLLAR holds a reservation on a 128-byte line and then
 * spins, waiting for someone else to change it. On real hardware a store by
 * ANY other processor to that line kills the reservation and raises
 * SPU_EVENT_LR on the reserving SPU, which is how a parked SPURS idle service
 * learns that the PPU has posted work. Emulated, the two halves live on
 * different host threads with nothing between them, so the SPU half alone gets
 * PUTLLC right (it compares the line before committing) while the PPU half
 * never tells anyone it wrote. The service sleeps through the signal.
 *
 * This is the missing half. Every line an SPU reserves is marked in a bitmap;
 * the PPU store paths (runtime/ppu/ppu_memory.h and runtime/ppu/ppu_loader.cpp)
 * check the bitmap, and on a hit commit the store under the same lock-line lock
 * the SPU's GETLLAR/PUTLLC transaction takes, then notify. That does two things
 * at once: the store can no longer land inside a PUTLLC's compare/commit window
 * (where it would be silently reverted), and the reserving SPU gets its event.
 *
 * The bitmap check is an atomic load against a global that stays zero until an
 * SPU actually reserves something, so a title that never runs SPU code avoids
 * touching the 4 MiB map. The optional PPU accessor predicate uses the same
 * atomic state and remains disabled unless explicitly selected at startup.
 */

#ifndef SPU_COHERENCY_H
#define SPU_COHERENCY_H

#include "spu_context.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPU external event: lock-line reservation lost. */
#define SPU_EVENT_LR  0x400u

/* The lock-line lock. One process-wide spinlock serializing every lock-line
 * transaction, SPU and PPU alike: the SPU's GETLLAR snapshot and PUTLLC
 * compare-and-commit, and a PPU store or stwcx to a line some SPU has
 * reserved. The critical sections are two 128-byte memcpy and a memcmp. */
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
/* Opt-in HLE queue wake acceleration, called with the lock-line lock held. */
#ifdef _WIN32
int spu_coh_watch_queue(uint32_t ea, void* host_address);
void spu_coh_unwatch_queue(int cookie);
/* Experimental SPURS task readiness watch. Caller holds the lock-line lock.
 * `host_address` is the 128-byte taskset header and event is an auto-reset
 * process-lifetime event. A collision/full table returns 0 (poll fallback). */
int spu_coh_watch_taskset(uint32_t taskset_ea, uint32_t spurs_ea,
                          void* host_address, void* event,
                          uint64_t (*qpc_callback)(void));
void spu_coh_unwatch_tasksets(uint32_t spurs_ea);
uint64_t spu_coh_taskset_ready_qpc_locked(uint32_t taskset_ea, uint32_t taskid);
#endif

/* Mark `ea`'s 128-byte line as SPU-reserved and remember `ctx` as a possible
 * reservation holder. Called from the SPU's GETLLAR with the lock held.
 *
 * Lines are marked and never unmarked. A line the SPURS kernel reserves is
 * reserved again microseconds later, so unmarking would buy nothing; a stale
 * mark costs one locked PPU store to a line the kernel owns anyway. */
void spu_coh_reserve(spu_context* ctx, uint32_t ea);

/* Is `addr`'s line marked? The PPU store fast path -- a range-free O(1) bitmap
 * lookup guarded by a global that is zero until the first GETLLAR. */
int spu_coh_is_reserved(uint32_t addr);

/* The experimental PPU accessor predicate uses the same bitmap as the
 * ABI helper above. Keep the representation C/C++ ABI-compatible; all accesses
 * to these objects, including the reference helper, use compiler atomics. */
#define SPU_COH_LINE_SHIFT 7u
#define SPU_COH_BITMAP_SIZE (1u << (32u - SPU_COH_LINE_SHIFT - 3u))
extern unsigned char spu_coh_reservation_bitmap[SPU_COH_BITMAP_SIZE];
extern uint32_t spu_coh_reservations_armed;

/* Keep the bitmap's storage representation plain across C/C++ translation
 * units. Clang/clang-cl use __atomic builtins; native MSVC uses its interlocked
 * intrinsics, whose operations are sequentially consistent. */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline uint32_t spu_coh_atomic_load_u32(const uint32_t* p)
{
    return (uint32_t)_InterlockedCompareExchange((volatile long*)p, 0, 0);
}
static inline unsigned char spu_coh_atomic_load_u8(const unsigned char* p)
{
    return (unsigned char)_InterlockedCompareExchange8((volatile char*)p, 0, 0);
}
static inline unsigned char spu_coh_atomic_load_u8_relaxed(const unsigned char* p)
{
    /* spu_coh_reserve holds the global writer lock; this byte is only read here. */
    return *(const volatile unsigned char*)p;
}
static inline void spu_coh_atomic_store_u32(uint32_t* p, uint32_t value)
{
    (void)_InterlockedExchange((volatile long*)p, (long)value);
}
static inline void spu_coh_atomic_store_u8(unsigned char* p, unsigned char value)
{
    (void)_InterlockedExchange8((volatile char*)p, (char)value);
}
#else
static inline uint32_t spu_coh_atomic_load_u32(const uint32_t* p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline unsigned char spu_coh_atomic_load_u8(const unsigned char* p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline unsigned char spu_coh_atomic_load_u8_relaxed(const unsigned char* p)
{ return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void spu_coh_atomic_store_u32(uint32_t* p, uint32_t value)
{ __atomic_store_n(p, value, __ATOMIC_RELEASE); }
static inline void spu_coh_atomic_store_u8(unsigned char* p, unsigned char value)
{ __atomic_store_n(p, value, __ATOMIC_RELEASE); }
#endif

/* Atomic reads are deliberate: PPU stores do not take the lock unless this
 * predicate says the line is reserved. Acquire loads prevent compiler caching
 * and pair with completed release publications; a check overlapping a writer
 * may still observe the preceding value, as in the existing protocol. */
static inline int spu_coh_is_reserved_inline(uint32_t addr)
{
    if (!spu_coh_atomic_load_u32(&spu_coh_reservations_armed)) return 0;
    uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    unsigned char bits = spu_coh_atomic_load_u8(&spu_coh_reservation_bitmap[line >> 3]);
    return (bits >> (line & 7u)) & 1u;
}

/* A store by the PPU landed on `ea`'s line. Raise SPU_EVENT_LR on every SPU
 * whose live reservation covers it, drop that reservation, and wake the host
 * thread in case it is parked in `rdch SPU_RdEventStat`.
 *
 * Expects the lock-line lock held, so that the store this is reporting and the
 * reservation state it clears are seen by an SPU's PUTLLC as one transaction. */
void spu_coh_notify_write(uint32_t ea);
/* Remove a context from the reserving set. MUST be called before a
 * transient (stack-allocated) spu_context goes out of scope. */
void spu_coh_unregister(spu_context* ctx);
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi);  /* thread exit */
/* As above, but leaves `self`'s own reservation alone: an SPU's own MFC
 * write does not cost it its reservation on hardware. */
void spu_coh_notify_write_except(uint32_t ea, const void* self);

/* Count of LR events delivered. A parked SPURS service that never wakes with
 * this at zero is a coherence miss; nonzero moves the question downstream. */
extern unsigned long g_spu_lr_raise;

#ifdef __cplusplus
}
#endif

#endif /* SPU_COHERENCY_H */
