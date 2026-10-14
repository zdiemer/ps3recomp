/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is a bit per
 * 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so context bookkeeping is serialized. The reservation bitmap is updated
 * atomically because spu_coh_is_reserved reads it without the lock: it is on
 * every PPU store, and must observe fresh publications without taking the
 * lock on the unreserved path.
 */

#include <stdlib.h>
#include "spu_coherency.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native HLE queues may sleep on their raw eight-byte BE header. All registry
 * access, snapshots and notifications occur under the existing lock-line lock.
 * Watches are reference counted and removed before their HLE calls return, so
 * freed queues leave no host pointers. Collisions retain timeout polling. */
#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "synchronization.lib")
static struct {
    uint32_t line;
    void* host;
    uint64_t value;
    unsigned references;
} s_queue_watches[1024];
static unsigned s_queue_watch_count;
static unsigned queue_watch_index(uint32_t ea)
{ return ((ea >> 7) ^ (ea >> 17)) & 1023u; }
int spu_coh_watch_queue(uint32_t ea, void* host_address)
{
    unsigned slot = queue_watch_index(ea);
    if (s_queue_watches[slot].references) {
        if (s_queue_watches[slot].line != (ea & ~127u) ||
            s_queue_watches[slot].host != host_address) return -1;
        ++s_queue_watches[slot].references;
        return (int)slot;
    }
    s_queue_watches[slot].line = ea & ~127u;
    s_queue_watches[slot].host = host_address;
    memcpy(&s_queue_watches[slot].value, host_address, 8);
    s_queue_watches[slot].references = 1;
    ++s_queue_watch_count;
    return (int)slot;
}
void spu_coh_unwatch_queue(int cookie)
{
    if (cookie < 0 || cookie >= 1024 || !s_queue_watches[cookie].references) return;
    if (!--s_queue_watches[cookie].references) {
        s_queue_watches[cookie].host = NULL;
        --s_queue_watch_count;
    }
}
static void queue_notify_publication(uint32_t ea)
{
    if (!s_queue_watch_count) return;
    unsigned slot = queue_watch_index(ea);
    if (s_queue_watches[slot].host && s_queue_watches[slot].line == (ea & ~127u)) {
        uint64_t value;
        memcpy(&value, s_queue_watches[slot].host, 8);
        if (value != s_queue_watches[slot].value) {
            s_queue_watches[slot].value = value;
            WakeByAddressAll(s_queue_watches[slot].host);
        }
    }
}

/* BEGIN SPU TASKSET WAKE WATCH */
/* SPURS readiness is published into the first taskset header line. Keep this
 * exact-line watcher separate from the HLE task scan: notifications only set
 * an auto-reset event, while spu_taskset_start_ready remains authoritative
 * for validation and claiming. Registry and per-task timestamps are protected
 * by the lock-line lock already held by every notify caller. */
#define SPU_TASKSET_WAKE_WATCHES 64u
#define SPU_TASKSET_READY_OFF 0x10u
#define SPU_TASKSET_PENDING_READY_OFF 0x20u
#define SPU_TASKSET_ENABLED_OFF 0x30u
typedef struct {
    uint32_t taskset_ea;
    uint32_t spurs_ea;
    uint8_t* host;
    HANDLE event;
    uint64_t (*qpc_callback)(void);
    uint8_t eligible[16];
    uint64_t ready_qpc[128];
} spu_taskset_wake_watch;
static spu_taskset_wake_watch s_taskset_watches[SPU_TASKSET_WAKE_WATCHES];
static unsigned s_taskset_watch_count;
static unsigned taskset_watch_index(uint32_t ea)
{ return ((ea >> 7) ^ (ea >> 15) ^ (ea >> 23)) & (SPU_TASKSET_WAKE_WATCHES - 1u); }
static void taskset_watch_prime(spu_taskset_wake_watch* w)
{
    for (unsigned b = 0; b < 16; ++b)
        w->eligible[b] = w->host[SPU_TASKSET_ENABLED_OFF + b] &
            (w->host[SPU_TASKSET_READY_OFF + b] | w->host[SPU_TASKSET_PENDING_READY_OFF + b]);
}
int spu_coh_watch_taskset(uint32_t taskset_ea, uint32_t spurs_ea,
                          void* host_address, void* event,
                          uint64_t (*qpc_callback)(void))
{
    if (!taskset_ea || (taskset_ea & 127u) || !spurs_ea || !host_address || !event) return 0;
    unsigned slot = taskset_watch_index(taskset_ea);
    spu_taskset_wake_watch* w = &s_taskset_watches[slot];
    if (w->taskset_ea == taskset_ea) {
        /* Reinitialization may reuse the guest EA with a new host mapping or
         * SPURS instance. Reset edge history so a stale ready bit cannot wake
         * the new owner. */
        if (w->spurs_ea != spurs_ea || w->host != (uint8_t*)host_address ||
            w->event != (HANDLE)event || w->qpc_callback != qpc_callback) {
            memset(w, 0, sizeof(*w));
            w->taskset_ea = taskset_ea; w->spurs_ea = spurs_ea;
            w->host = (uint8_t*)host_address; w->event = (HANDLE)event;
            w->qpc_callback = qpc_callback;
            taskset_watch_prime(w);
        }
        return 1;
    }
    if (w->taskset_ea) return 0; /* exact-line collision: retain polling */
    w->taskset_ea = taskset_ea; w->spurs_ea = spurs_ea;
    w->host = (uint8_t*)host_address; w->event = (HANDLE)event;
    w->qpc_callback = qpc_callback;
    ++s_taskset_watch_count;
    taskset_watch_prime(w);
    return 1;
}
void spu_coh_unwatch_tasksets(uint32_t spurs_ea)
{
    if (!spurs_ea) return;
    for (unsigned i = 0; i < SPU_TASKSET_WAKE_WATCHES; ++i)
        if (s_taskset_watches[i].spurs_ea == spurs_ea) {
            memset(&s_taskset_watches[i], 0, sizeof(s_taskset_watches[i]));
            --s_taskset_watch_count;
        }
}
uint64_t spu_coh_taskset_ready_qpc_locked(uint32_t taskset_ea, uint32_t taskid)
{
    if (taskid >= 128) return 0;
    spu_taskset_wake_watch* w = &s_taskset_watches[taskset_watch_index(taskset_ea)];
    return w->taskset_ea == taskset_ea ? w->ready_qpc[taskid] : 0;
}
static void taskset_notify_publication(uint32_t ea)
{
    if (!s_taskset_watch_count) return;
    uint32_t line = ea & ~127u;
    unsigned slot = taskset_watch_index(line);
    {
        uint8_t* host = s_taskset_watches[slot].host;
        if (!host || (s_taskset_watches[slot].taskset_ea & ~127u) != line) return;
        int rose = 0;
        uint8_t next[16];
        for (unsigned b = 0; b < 16; ++b) {
            next[b] = host[SPU_TASKSET_ENABLED_OFF + b] &
                      (host[SPU_TASKSET_READY_OFF + b] | host[SPU_TASKSET_PENDING_READY_OFF + b]);
            if (next[b] & (uint8_t)~s_taskset_watches[slot].eligible[b]) rose = 1;
        }
        if (rose) {
            uint64_t qpc = s_taskset_watches[slot].qpc_callback
                         ? s_taskset_watches[slot].qpc_callback() : 0;
            for (unsigned b = 0; b < 16; ++b) {
                uint8_t rising = next[b] & (uint8_t)~s_taskset_watches[slot].eligible[b];
                for (unsigned bit = 0; bit < 8; ++bit) {
                    if (rising & (0x80u >> bit)) {
                        s_taskset_watches[slot].ready_qpc[b * 8 + bit] = qpc;
                    }
                }
            }
            SetEvent(s_taskset_watches[slot].event);
        }
        /* Update on both rises and falls, so only a fresh eligibility edge
         * can signal and stale ready timestamps do not survive deactivation. */
        for (unsigned b = 0; b < 16; ++b) {
            uint8_t fallen = s_taskset_watches[slot].eligible[b] & (uint8_t)~next[b];
            for (unsigned bit = 0; bit < 8; ++bit)
                if (fallen & (0x80u >> bit)) s_taskset_watches[slot].ready_qpc[b * 8 + bit] = 0;
            s_taskset_watches[slot].eligible[b] = next[b];
        }
    }
}
/* END SPU TASKSET WAKE WATCH */
#else
#define queue_notify_publication(ea) ((void)0)
#define taskset_notify_publication(ea) ((void)0)
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * spinlock, same semantics, now shared. _InterlockedExchange is a clang-cl/MSVC
 * intrinsic (no runtime library symbol needed); elsewhere use the C11
 * equivalent, which lowers to the same LL/SC or lock-xchg on every target.
 * ===========================================================================*/
#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
static volatile long g_lockline = 0;
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time. */
void spu_lockline_lock(void)
{
    unsigned spins = 0;
    while (_InterlockedExchange(&g_lockline, 1)) {
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); }
        } while (g_lockline);
    }
}
void spu_lockline_unlock(void) { _InterlockedExchange(&g_lockline, 0); }
#else
#include <stdatomic.h>
static atomic_flag g_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
#endif

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
unsigned char spu_coh_reservation_bitmap[SPU_COH_BITMAP_SIZE];

/* Zero until the first GETLLAR anywhere. Keeps the PPU fast path off the
 * bitmap entirely for a title that runs no SPU code. Published atomically after
 * the first line bit so an acquire-in-program-order first observation sees the
 * mark. A check that overlaps that first publication may still linearize just
 * before it and miss that one reservation; the existing protocol does not
 * serialize an unreserved PPU store against a concurrent first reservation. */
uint32_t spu_coh_reservations_armed;

/* Compact live set, always accessed under the lock-line lock. Async jobs may
 * outnumber physical SPUs, so an eight-entry ceiling can lose real holders. */
static spu_context** s_coh_ctxs;
static size_t s_coh_count, s_coh_capacity;

unsigned long g_spu_lr_raise = 0;

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    /* Reservation publishers hold the lock-line lock, so they can preserve
     * neighboring bits with an ordinary read followed by an atomic release
     * store. The PPU predicate reads this byte concurrently without that lock. */
    unsigned char* byte = &spu_coh_reservation_bitmap[line >> 3];
    unsigned char bits = spu_coh_atomic_load_u8_relaxed(byte);
    bits |= (unsigned char)(1u << (line & 7));
    spu_coh_atomic_store_u8(byte, bits);
    spu_coh_atomic_store_u32(&spu_coh_reservations_armed, 1u);

    if (!ctx) return;
    /* Search the whole live set before inserting. Filling an early hole
     * before finding an existing later entry registered the same object twice;
     * unregister then left a dangling pointer after its first removal. */
    for (size_t i = 0; i < s_coh_count; i++) {
        if (s_coh_ctxs[i] == ctx) return;
    }
    if (s_coh_count == s_coh_capacity) {
        size_t capacity = s_coh_capacity ? s_coh_capacity * 2 : 8;
        if (capacity < s_coh_capacity || capacity > SIZE_MAX / sizeof(*s_coh_ctxs)) abort();
        spu_context** holders = (spu_context**)realloc(s_coh_ctxs, capacity * sizeof(*holders));
        if (!holders) { fputs("[spu-coh] cannot retain reservation holders\n",stderr); abort(); }
        s_coh_ctxs = holders; s_coh_capacity = capacity;
    }
    s_coh_ctxs[s_coh_count++] = ctx;
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv_valid, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    /* Under the lock-line lock, which every walker of s_coh_ctxs holds: a
     * notifier that loaded this pointer just before removal would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    for (size_t i = 0; i < s_coh_count;)
        if (s_coh_ctxs[i] == ctx) s_coh_ctxs[i] = s_coh_ctxs[--s_coh_count];
        else ++i;
    spu_lockline_unlock();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    spu_lockline_lock();
    for (size_t i = 0; i < s_coh_count;) {
        uintptr_t c = (uintptr_t)s_coh_ctxs[i];
        if (c >= lo && c < hi) {
            static int _n = 0;
            if (_n++ < 16)
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, s_coh_ctxs[i]->image_id);
            s_coh_ctxs[i] = s_coh_ctxs[--s_coh_count];
        } else ++i;
    }
    spu_lockline_unlock();
}

int spu_coh_is_reserved(uint32_t addr)
{
    return spu_coh_is_reserved_inline(addr);
}

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    queue_notify_publication(ea);
    taskset_notify_publication(ea);
    uint32_t line = ea & ~127u;
    for (size_t i = 0; i < s_coh_count; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c || (const void*)c == self) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
    }
}

void spu_coh_notify_write(uint32_t ea)
{
    queue_notify_publication(ea);
    taskset_notify_publication(ea);
    uint32_t line = ea & ~127u;
    for (size_t i = 0; i < s_coh_count; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;          /* reservation lost, PUTLLC must fail */
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            g_spu_lr_raise++;
            /* The event_status store has to be visible before the wake. The
             * waiter re-polls its predicate on a timeout as well, so a
             * straggling store costs latency and not the wakeup itself. */
            spu_ch_wake(c);
        }
    }
}

#ifdef __cplusplus
}
#endif
