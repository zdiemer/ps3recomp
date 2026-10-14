/* Schedule guest-created tasks while persistent controller tasks are alive.
 * Metadata and reservations share the SPU lock-line lock, so host claims cannot
 * race a guest PUTLLC. Image lookup and host thread creation happen unlocked. */
#include "spu_workload.h"
#include "spu_coherency.h"
#include "../ps3_log.h"
#include "../../libs/spurs/spurs_taskset.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#define SPU_WAKE_INSTANCES 8u
static struct { uint32_t ea; void* event; } s_spurs_wake[SPU_WAKE_INSTANCES];
static volatile LONG s_spurs_wake_active;
void spu_taskset_wake_register(uint32_t spurs_ea, void* event)
{
    if (!spurs_ea || !event) return;
    spu_lockline_lock();
    /* Initialize may reuse the same guest EA, host mapping and process-lifetime
     * event. Drop old taskset snapshots explicitly so the new instance's ready
     * bits are primed from its current memory image. */
    spu_coh_unwatch_tasksets(spurs_ea);
    unsigned free_slot = SPU_WAKE_INSTANCES;
    for (unsigned i = 0; i < SPU_WAKE_INSTANCES; ++i) {
        if (s_spurs_wake[i].ea == spurs_ea) { free_slot = i; break; }
        if (!s_spurs_wake[i].ea && free_slot == SPU_WAKE_INSTANCES) free_slot = i;
    }
    if (free_slot < SPU_WAKE_INSTANCES) {
        if (!s_spurs_wake[free_slot].ea) InterlockedExchange(&s_spurs_wake_active, 1);
        s_spurs_wake[free_slot].ea = spurs_ea;
        s_spurs_wake[free_slot].event = event;
    }
    spu_lockline_unlock();
}
void spu_taskset_wake_unregister(uint32_t spurs_ea)
{
    if (!spurs_ea) return;
    spu_lockline_lock();
    for (unsigned i = 0; i < SPU_WAKE_INSTANCES; ++i)
        if (s_spurs_wake[i].ea == spurs_ea) memset(&s_spurs_wake[i], 0, sizeof(s_spurs_wake[i]));
    LONG active = 0;
    for (unsigned i = 0; i < SPU_WAKE_INSTANCES; ++i) active |= s_spurs_wake[i].ea != 0;
    InterlockedExchange(&s_spurs_wake_active, active);
    spu_coh_unwatch_tasksets(spurs_ea);
    spu_lockline_unlock();
}
static void* taskset_wake_event_locked(uint32_t spurs_ea)
{
    for (unsigned i = 0; i < SPU_WAKE_INSTANCES; ++i)
        if (s_spurs_wake[i].ea == spurs_ea) return s_spurs_wake[i].event;
    return NULL;
}
#else
void spu_taskset_wake_register(uint32_t spurs_ea, void* event) { (void)spurs_ea; (void)event; }
void spu_taskset_wake_unregister(uint32_t spurs_ea) { (void)spurs_ea; }
#endif

extern uint8_t* vm_base;
#define TS_TRACK 16
static struct { uint32_t ea; uint8_t running[16]; } s_ts_run[TS_TRACK];

/* BEGIN SPU TASKSET SCAN PROFILE */
/* Explicit diagnostic only: no file I/O on a scan or guest lock path. The
 * normal-close caller invokes spu_taskset_scan_profile_dump(). This is
 * compiled out of ordinary builds; profiling builds stay clock-free unless
 * the dedicated CSV path is configured. */
#if defined(YZ_PERF_PROFILE) && defined(_WIN32)
#ifndef SPU_TASKSET_SCAN_PROFILE_CAPACITY
#define SPU_TASKSET_SCAN_PROFILE_CAPACITY 131072u
#endif
#ifndef SPU_TASKSET_SCAN_PROFILE_PATH_MAX
#define SPU_TASKSET_SCAN_PROFILE_PATH_MAX 4096u
#endif
#ifndef SPU_TASKSET_SCAN_PROFILE_DUMP_TIMEOUT_MS
#define SPU_TASKSET_SCAN_PROFILE_DUMP_TIMEOUT_MS 2000u
#endif
typedef struct {
    volatile LONG busy;
    volatile LONG64 published; /* ticket + 1, zero means unused */
    uint64_t ticket;
    uint64_t begin_qpc, end_qpc;
    uint64_t total_cycles, prep_cycles;
    uint64_t total_ticks, prep_ticks, watch_ticks;
    uint32_t taskset_ea, dispatch_cause, host_tid;
    uint16_t eligible, claimed, dispatch_attempts, dispatch_ok, dispatch_failed;
    uint8_t qpc_valid, total_cycles_valid, prep_cycles_valid, reserved;
} spu_taskset_scan_profile_record;
static spu_taskset_scan_profile_record s_scan_profile_ring[SPU_TASKSET_SCAN_PROFILE_CAPACITY];
static volatile LONG s_scan_profile_init;
static volatile LONG s_scan_profile_enabled;
static volatile LONG s_scan_profile_accepting;
static volatile LONG s_scan_profile_writers;
static volatile LONG s_scan_profile_dumping;
static volatile LONG s_scan_profile_frozen;
static volatile LONG64 s_scan_profile_next_ticket;
static volatile LONG64 s_scan_profile_overwritten;
static volatile LONG64 s_scan_profile_dropped_busy;
static volatile LONG64 s_scan_profile_dropped_stale;
static volatile LONG64 s_scan_profile_max_discarded_end;
static volatile LONG64 s_scan_profile_discarded_qpc_invalid;
static char s_scan_profile_path[SPU_TASKSET_SCAN_PROFILE_PATH_MAX];
static uint64_t s_scan_profile_frequency;
static uint64_t s_scan_profile_capture_start_qpc;
static int s_scan_profile_capture_start_valid;
static uint64_t s_scan_profile_capture_end_qpc;
static int s_scan_profile_capture_end_valid;

typedef struct {
    uint64_t begin_qpc, begin_cycles;
    uint64_t prep_ticks, prep_cycles, watch_ticks;
    uint32_t taskset_ea, dispatch_cause, host_tid;
    uint16_t eligible, claimed, dispatch_attempts, dispatch_ok, dispatch_failed;
    uint8_t qpc_valid, total_cycles_valid, prep_cycles_valid;
} spu_taskset_scan_profile_scope;

#ifdef SPU_TASKSET_SCAN_PROFILE_TEST_HOOK_AFTER_BUSY
#define SCAN_PROFILE_TEST_AFTER_BUSY(slot, ticket) \
    SPU_TASKSET_SCAN_PROFILE_TEST_HOOK_AFTER_BUSY((slot), (ticket))
#else
#define SCAN_PROFILE_TEST_AFTER_BUSY(slot, ticket) ((void)0)
#endif

static int scan_profile_qpc(uint64_t* value)
{
    LARGE_INTEGER now;
    if (!value || !QueryPerformanceCounter(&now) || now.QuadPart < 0) return 0;
    *value = (uint64_t)now.QuadPart;
    return 1;
}

static int scan_profile_cycles(uint64_t* value)
{
    ULONG64 cycles;
    if (!value || !QueryThreadCycleTime(GetCurrentThread(), &cycles)) return 0;
    *value = (uint64_t)cycles;
    return 1;
}

static void scan_profile_initialize(void)
{
    if (s_scan_profile_init == 2) return;
    LONG state = InterlockedCompareExchange(&s_scan_profile_init, 1, 0);
    if (state == 0) {
        const char* path = getenv("SPU_TASKSET_SCAN_PROFILE_CSV");
        LARGE_INTEGER frequency;
        if (path && *path && strlen(path) < sizeof(s_scan_profile_path) &&
            QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
            memcpy(s_scan_profile_path, path, strlen(path) + 1);
            s_scan_profile_frequency = (uint64_t)frequency.QuadPart;
            InterlockedExchange(&s_scan_profile_accepting, 1);
            s_scan_profile_capture_start_valid = scan_profile_qpc(&s_scan_profile_capture_start_qpc);
            if (!s_scan_profile_capture_start_valid)
                InterlockedExchange(&s_scan_profile_accepting, 0);
            /* Publish only after every immutable selector/bound is initialized. */
            InterlockedExchange(&s_scan_profile_enabled, 1);
        }
        InterlockedExchange(&s_scan_profile_init, 2);
    } else {
        while (InterlockedCompareExchange(&s_scan_profile_init, 2, 2) != 2)
            YieldProcessor();
    }
}

static int scan_profile_is_active(void)
{
    scan_profile_initialize();
    return s_scan_profile_enabled && s_scan_profile_accepting;
}

static int scan_profile_begin(spu_taskset_scan_profile_scope* scope,
                              uint32_t taskset_ea, uint32_t cause)
{
    if (!scope) return 0;
    if (!InterlockedCompareExchange(&s_scan_profile_accepting, 1, 1)) return 0;
    InterlockedIncrement(&s_scan_profile_writers);
    if (!InterlockedCompareExchange(&s_scan_profile_accepting, 1, 1)) {
        InterlockedDecrement(&s_scan_profile_writers);
        return 0;
    }
    memset(scope, 0, sizeof(*scope));
    scope->taskset_ea = taskset_ea;
    scope->dispatch_cause = cause;
    scope->host_tid = GetCurrentThreadId();
    scope->qpc_valid = (uint8_t)scan_profile_qpc(&scope->begin_qpc);
    scope->total_cycles_valid = (uint8_t)scan_profile_cycles(&scope->begin_cycles);
    scope->prep_cycles_valid = 1;
    return 1;
}

static void scan_profile_pause(spu_taskset_scan_profile_scope* scope,
                               uint64_t* qpc, uint64_t* cycles,
                               uint8_t* qpc_valid, uint8_t* cycles_valid)
{
    if (!scope) return;
    *qpc_valid = (uint8_t)scan_profile_qpc(qpc);
    *cycles_valid = (uint8_t)scan_profile_cycles(cycles);
}

static void scan_profile_resume(spu_taskset_scan_profile_scope* scope,
                                uint64_t qpc_begin, uint64_t cycles_begin,
                                uint8_t qpc_valid, uint8_t cycles_valid)
{
    uint64_t qpc_end, cycles_end;
    if (!scope) return;
    if (!qpc_valid || !scan_profile_qpc(&qpc_end) || qpc_end < qpc_begin)
        scope->qpc_valid = 0;
    else
        scope->prep_ticks += qpc_end - qpc_begin;
    if (!cycles_valid || !scan_profile_cycles(&cycles_end) || cycles_end < cycles_begin)
        scope->prep_cycles_valid = 0;
    else
        scope->prep_cycles += cycles_end - cycles_begin;
}

static void scan_profile_note_eligible(spu_taskset_scan_profile_scope* scope)
{
    if (scope && scope->eligible != UINT16_MAX) ++scope->eligible;
}

static void scan_profile_note_claimed(spu_taskset_scan_profile_scope* scope)
{
    if (scope && scope->claimed != UINT16_MAX) ++scope->claimed;
}

static void scan_profile_note_dispatch(spu_taskset_scan_profile_scope* scope, int ok)
{
    if (!scope) return;
    if (scope->dispatch_attempts != UINT16_MAX) ++scope->dispatch_attempts;
    if (ok) {
        if (scope->dispatch_ok != UINT16_MAX) ++scope->dispatch_ok;
    } else if (scope->dispatch_failed != UINT16_MAX) ++scope->dispatch_failed;
}

static void scan_profile_note_discarded_end(uint64_t end_qpc, int valid)
{
    if (!valid) {
        InterlockedIncrement64(&s_scan_profile_discarded_qpc_invalid);
        return;
    }
    LONG64 prior = InterlockedCompareExchange64(&s_scan_profile_max_discarded_end, 0, 0);
    while ((uint64_t)prior < end_qpc) {
        LONG64 seen = InterlockedCompareExchange64(&s_scan_profile_max_discarded_end,
                                                   (LONG64)end_qpc, prior);
        if (seen == prior) break;
        prior = seen;
    }
}

static void scan_profile_finish(spu_taskset_scan_profile_scope* scope)
{
    uint64_t end_qpc = 0, end_cycles = 0;
    int end_qpc_valid, end_cycles_valid;
    spu_taskset_scan_profile_record value;
    if (!scope) return;
    end_qpc_valid = scan_profile_qpc(&end_qpc);
    end_cycles_valid = scan_profile_cycles(&end_cycles);
    memset(&value, 0, sizeof(value));
    value.begin_qpc = scope->begin_qpc;
    value.end_qpc = end_qpc;
    value.total_ticks = end_qpc_valid && scope->qpc_valid && end_qpc >= scope->begin_qpc
                      ? end_qpc - scope->begin_qpc : 0;
    value.prep_ticks = scope->prep_ticks;
    value.watch_ticks = scope->watch_ticks;
    value.qpc_valid = (uint8_t)(end_qpc_valid && scope->qpc_valid &&
                                end_qpc >= scope->begin_qpc &&
                                scope->prep_ticks <= value.total_ticks);
    value.total_cycles_valid = (uint8_t)(end_cycles_valid && scope->total_cycles_valid &&
                                         end_cycles >= scope->begin_cycles);
    value.total_cycles = value.total_cycles_valid ? end_cycles - scope->begin_cycles : 0;
    value.prep_cycles = scope->prep_cycles;
    value.prep_cycles_valid = (uint8_t)(scope->prep_cycles_valid &&
                                        value.total_cycles_valid &&
                                        scope->prep_cycles <= value.total_cycles);
    value.taskset_ea = scope->taskset_ea;
    value.dispatch_cause = scope->dispatch_cause;
    value.host_tid = scope->host_tid;
    value.eligible = scope->eligible;
    value.claimed = scope->claimed;
    value.dispatch_attempts = scope->dispatch_attempts;
    value.dispatch_ok = scope->dispatch_ok;
    value.dispatch_failed = scope->dispatch_failed;

    int64_t ticket = InterlockedIncrement64(&s_scan_profile_next_ticket) - 1;
    value.ticket = (uint64_t)ticket;
    spu_taskset_scan_profile_record* slot =
        &s_scan_profile_ring[(uint64_t)ticket % SPU_TASKSET_SCAN_PROFILE_CAPACITY];
    if (InterlockedCompareExchange(&slot->busy, 1, 0) != 0) {
        InterlockedIncrement64(&s_scan_profile_dropped_busy);
        scan_profile_note_discarded_end(end_qpc, end_qpc_valid);
    } else {
        SCAN_PROFILE_TEST_AFTER_BUSY(slot, value.ticket);
        uint64_t prior = (uint64_t)InterlockedCompareExchange64(&slot->published, 0, 0);
        if (prior && prior - 1 > value.ticket) {
            InterlockedIncrement64(&s_scan_profile_dropped_stale);
            scan_profile_note_discarded_end(end_qpc, end_qpc_valid);
        } else {
            if (prior) {
                InterlockedIncrement64(&s_scan_profile_overwritten);
                scan_profile_note_discarded_end(slot->end_qpc, slot->qpc_valid);
            }
            slot->ticket = value.ticket;
            slot->begin_qpc = value.begin_qpc;
            slot->end_qpc = value.end_qpc;
            slot->total_cycles = value.total_cycles;
            slot->prep_cycles = value.prep_cycles;
            slot->total_ticks = value.total_ticks;
            slot->prep_ticks = value.prep_ticks;
            slot->watch_ticks = value.watch_ticks;
            slot->taskset_ea = value.taskset_ea;
            slot->dispatch_cause = value.dispatch_cause;
            slot->host_tid = value.host_tid;
            slot->eligible = value.eligible;
            slot->claimed = value.claimed;
            slot->dispatch_attempts = value.dispatch_attempts;
            slot->dispatch_ok = value.dispatch_ok;
            slot->dispatch_failed = value.dispatch_failed;
            slot->qpc_valid = value.qpc_valid;
            slot->total_cycles_valid = value.total_cycles_valid;
            slot->prep_cycles_valid = value.prep_cycles_valid;
            InterlockedExchange64(&slot->published, ticket + 1);
        }
        InterlockedExchange(&slot->busy, 0);
    }
    InterlockedDecrement(&s_scan_profile_writers);
}

static int __cdecl scan_profile_ticket_compare(const void* left, const void* right)
{
    const uint64_t a = *(const uint64_t*)left;
    const uint64_t b = *(const uint64_t*)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

void spu_taskset_scan_profile_dump(void)
{
    /* Do not wait on another thread's first-use initializer during shutdown. */
    if (InterlockedCompareExchange(&s_scan_profile_init, 2, 2) != 2) return;
    if (!InterlockedCompareExchange(&s_scan_profile_enabled, 1, 1)) return;
    if (InterlockedCompareExchange(&s_scan_profile_dumping, 1, 0) != 0) return;
    if (!InterlockedCompareExchange(&s_scan_profile_frozen, 1, 1)) {
        s_scan_profile_capture_end_valid = scan_profile_qpc(&s_scan_profile_capture_end_qpc);
        InterlockedExchange(&s_scan_profile_accepting, 0);
        InterlockedExchange(&s_scan_profile_frozen, 1);
    }

    DWORD started = GetTickCount();
    int writers_drained = 0;
    for (;;) {
        if (InterlockedCompareExchange(&s_scan_profile_writers, 0, 0) == 0) {
            writers_drained = 1;
            break;
        }
        DWORD elapsed = GetTickCount() - started;
        if (elapsed >= SPU_TASKSET_SCAN_PROFILE_DUMP_TIMEOUT_MS) break;
        SwitchToThread();
    }

    LONG inflight = InterlockedCompareExchange(&s_scan_profile_writers, 0, 0);
    uint64_t drain_end_qpc = 0;
    int drain_end_valid = writers_drained && scan_profile_qpc(&drain_end_qpc);
    int complete = s_scan_profile_capture_end_valid &&
                   s_scan_profile_capture_start_valid && writers_drained && drain_end_valid;
    const char* failure_reason = !s_scan_profile_capture_start_valid ? "qpc_start" :
                                 !s_scan_profile_capture_end_valid ? "qpc_end" :
                                 !writers_drained ? "writer_timeout" :
                                 !drain_end_valid ? "qpc_drain" : "none";
    FILE* file = fopen(s_scan_profile_path, "wb");
    if (!file) {
        fprintf(stderr, "[spu-taskset-scan] failed to open %s\n", s_scan_profile_path);
        return;
    }
    uint64_t next = (uint64_t)InterlockedCompareExchange64(&s_scan_profile_next_ticket, 0, 0);
    uint64_t retained = 0;
    uint64_t* retained_tickets = NULL;
    if (complete) {
        for (uint32_t i = 0; i < SPU_TASKSET_SCAN_PROFILE_CAPACITY; ++i) {
            spu_taskset_scan_profile_record* slot = &s_scan_profile_ring[i];
            if ((uint64_t)InterlockedCompareExchange64(&slot->published, 0, 0)) ++retained;
        }
        if (retained) retained_tickets = (uint64_t*)malloc((size_t)retained * sizeof(*retained_tickets));
        if (retained && !retained_tickets) {
            complete = 0;
            retained = 0;
            failure_reason = "row_buffer";
        } else if (retained) {
            uint64_t cursor = 0;
            for (uint32_t i = 0; i < SPU_TASKSET_SCAN_PROFILE_CAPACITY; ++i) {
                spu_taskset_scan_profile_record* slot = &s_scan_profile_ring[i];
                uint64_t published = (uint64_t)InterlockedCompareExchange64(&slot->published, 0, 0);
                if (published) retained_tickets[cursor++] = published - 1;
            }
            retained = cursor;
            qsort(retained_tickets, (size_t)retained, sizeof(*retained_tickets),
                  scan_profile_ticket_compare);
        }
    }
    fprintf(file, "#qpc_frequency,%" PRIu64 ",capture_start_qpc,%" PRIu64
                  ",capture_end_qpc,%" PRIu64 ",drain_end_qpc,%" PRIu64
                  ",complete,%u,inflight,%ld"
                  ",failure_reason,%s,capacity,%u,next_ticket,%" PRIu64
                  ",retained_rows,%" PRIu64 ",overwritten,%" PRIu64
                  ",dropped_busy,%" PRIu64
                  ",dropped_stale,%" PRIu64 ",max_discarded_end_qpc,%" PRIu64
                  ",discarded_qpc_invalid,%" PRIu64 "\n",
            s_scan_profile_frequency, s_scan_profile_capture_start_qpc,
            s_scan_profile_capture_end_qpc, drain_end_qpc, complete, inflight, failure_reason,
            SPU_TASKSET_SCAN_PROFILE_CAPACITY, next, retained,
            (uint64_t)InterlockedCompareExchange64(&s_scan_profile_overwritten, 0, 0),
            (uint64_t)InterlockedCompareExchange64(&s_scan_profile_dropped_busy, 0, 0),
            (uint64_t)InterlockedCompareExchange64(&s_scan_profile_dropped_stale, 0, 0),
            (uint64_t)InterlockedCompareExchange64(&s_scan_profile_max_discarded_end, 0, 0),
            (uint64_t)InterlockedCompareExchange64(&s_scan_profile_discarded_qpc_invalid, 0, 0));
    fputs("ticket,taskset_ea,dispatch_cause,host_tid,begin_qpc,end_qpc,qpc_valid,total_ticks,prep_ticks,scan_ticks,total_thread_cycles,prep_thread_cycles,scan_thread_cycles,total_cycles_valid,prep_cycles_valid,watch_ticks,slots,eligible,claimed,dispatch_attempts,dispatch_ok,dispatch_failed\n", file);
    /* A ticket can be dropped while an older writer still publishes its slot.
     * Export all retained tickets after quiescence instead of deriving a lossy
     * [next-capacity,next) window from ticket reservation order. */
    if (complete) {
      for (uint64_t n = 0; n < retained; ++n) {
        uint64_t ticket = retained_tickets[n];
        spu_taskset_scan_profile_record* slot = &s_scan_profile_ring[ticket % SPU_TASKSET_SCAN_PROFILE_CAPACITY];
        uint64_t published = (uint64_t)InterlockedCompareExchange64(&slot->published, 0, 0);
        if (published != ticket + 1) continue;
        uint64_t scan_ticks = slot->qpc_valid ? slot->total_ticks - slot->prep_ticks : 0;
        uint64_t scan_cycles = slot->prep_cycles_valid && slot->total_cycles_valid
                             ? slot->total_cycles - slot->prep_cycles : 0;
        fprintf(file, "%" PRIu64 ",%08" PRIX32 ",%" PRIu32 ",%" PRIu32
                      ",%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                      ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%u,%" PRIu64
                      ",128,%u,%u,%u,%u,%u\n",
                slot->ticket, slot->taskset_ea, slot->dispatch_cause, slot->host_tid,
                slot->begin_qpc, slot->end_qpc, slot->qpc_valid, slot->total_ticks,
                slot->prep_ticks, scan_ticks, slot->total_cycles, slot->prep_cycles,
                scan_cycles, slot->total_cycles_valid, slot->prep_cycles_valid,
                slot->watch_ticks, slot->eligible, slot->claimed, slot->dispatch_attempts,
                slot->dispatch_ok, slot->dispatch_failed);
      }
    }
    free(retained_tickets);
    int failed = ferror(file);
    if (fclose(file) != 0) failed = 1;
    if (failed) fprintf(stderr, "[spu-taskset-scan] failed to write %s\n", s_scan_profile_path);
}

#else
void spu_taskset_scan_profile_dump(void) { }
#endif
/* END SPU TASKSET SCAN PROFILE */

static uint8_t* ts_running(uint32_t ea)
{
    for (unsigned i = 0; i < TS_TRACK; ++i)
        if (s_ts_run[i].ea == ea) return s_ts_run[i].running;
    for (unsigned i = 0; i < TS_TRACK; ++i)
        if (!s_ts_run[i].ea) { s_ts_run[i].ea = ea; return s_ts_run[i].running; }
    return NULL;
}

int spu_taskset_claim(uint32_t ea, uint32_t task)
{
    if (!ea || task >= 128) return 0;
    spu_lockline_lock();
    uint8_t* run = ts_running(ea);
    uint8_t mask = (uint8_t)(0x80u >> (task & 7));
    int claimed = run && !(run[task / 8] & mask);
    if (claimed) run[task / 8] |= mask;
    spu_lockline_unlock();
    return claimed;
}

void spu_taskset_release(uint32_t ea, uint32_t task)
{
    if (!ea || task >= 128) return;
    spu_lockline_lock();
    uint8_t* run = ts_running(ea);
    if (run) run[task / 8] &= (uint8_t)~(0x80u >> (task & 7));
    spu_lockline_unlock();
}

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

void spu_taskset_capture_args(uint32_t ea, uint32_t task, uint32_t args[4])
{
    spu_lockline_lock();
    const uint8_t* ti = vm_base + ea + CSTS_TASKINFO + TI_SIZE * task;
    for (unsigned i = 0; i < 4; ++i) args[i] = be32(ti + TI_ARGS + 4 * i);
    spu_lockline_unlock();
}

void spu_taskset_start_ready_cause(uint32_t ea, uint32_t dispatch_cause)
{
    if (!ea) return;
#if defined(YZ_PERF_PROFILE) && defined(_WIN32)
    int profile_active = scan_profile_is_active();
    uint64_t watch_begin_qpc = 0, watch_end_qpc = 0;
    int watch_begin_valid = 0, watch_end_valid = 0;
#endif
#ifdef _WIN32
    /* Register once per dispatch pass. Notifications and this snapshot share
     * the lock, so a readiness edge cannot be lost between priming and scan. */
    if (s_spurs_wake_active) {
#if defined(YZ_PERF_PROFILE)
        if (profile_active) watch_begin_valid = scan_profile_qpc(&watch_begin_qpc);
#endif
        spu_lockline_lock();
        uint8_t* initial_ts = vm_base + ea;
        uint32_t initial_owner = be32(initial_ts + CSTS_SPURS + 4);
        void* initial_event = taskset_wake_event_locked(initial_owner);
        if (initial_event)
            (void)spu_coh_watch_taskset(ea, initial_owner, initial_ts, initial_event,
                                        spu_workload_async_trace_mark);
        spu_lockline_unlock();
#if defined(YZ_PERF_PROFILE)
        if (profile_active) watch_end_valid = scan_profile_qpc(&watch_end_qpc);
#endif
    }
#endif
#if defined(YZ_PERF_PROFILE) && defined(_WIN32)
    spu_taskset_scan_profile_scope scan_profile;
    int scan_profile_enabled = profile_active &&
        scan_profile_begin(&scan_profile, ea, dispatch_cause);
    if (scan_profile_enabled && watch_begin_valid && watch_end_valid &&
        watch_end_qpc >= watch_begin_qpc)
        scan_profile.watch_ticks = watch_end_qpc - watch_begin_qpc;
#endif
    for (uint32_t task = 0; task < 128; ++task) {
        uint8_t mask = (uint8_t)(0x80u >> (task & 7));
        uint32_t elf = 0, context = 0;
        uint64_t claim_qpc = 0, ready_qpc = 0;
        spu_lockline_lock();
        uint8_t* ts = vm_base + ea;
        uint8_t* run = ts_running(ea);
        if (run && !(run[task / 8] & mask) &&
            (ts[CSTS_ENABLED + task / 8] & mask) &&
            ((ts[CSTS_READY + task / 8] | ts[CSTS_PENDING_READY + task / 8]) & mask)) {
#if defined(YZ_PERF_PROFILE) && defined(_WIN32)
            if (scan_profile_enabled) scan_profile_note_eligible(&scan_profile);
#endif
            const uint8_t* ti = ts + CSTS_TASKINFO + 0x30 * task;
            elf = be32(ti + TI_ELF + 4) & ~7u;
            context = be32(ti + TI_CONTEXT + 4) & ~127u;
            if (elf) {
                run[task / 8] |= mask; /* claim before unlocking/spawning */
 #ifdef _WIN32
                /* Read readiness publication timestamp under the same lock as
                 * the eligibility test/claim. Zero means unobserved/pre-watch. */
                ready_qpc = spu_coh_taskset_ready_qpc_locked(ea, task);
 #else
                ready_qpc = 0;
 #endif
                claim_qpc = spu_workload_async_trace_mark();
            }
        }
        spu_lockline_unlock();
        if (!elf) continue;
#if defined(YZ_PERF_PROFILE) && defined(_WIN32)
        if (scan_profile_enabled) scan_profile_note_claimed(&scan_profile);
        uint64_t prep_qpc = 0, prep_cycles = 0;
        uint8_t prep_qpc_valid = 0, prep_cycles_valid = 0;
        if (scan_profile_enabled)
            scan_profile_pause(&scan_profile, &prep_qpc, &prep_cycles,
                               &prep_qpc_valid, &prep_cycles_valid);
#endif
        size_t size = spu_elf_image_size(vm_base + elf, 2u * 1024 * 1024);
        int dispatched = size && spu_workload_dispatch_async_task_provenance(
                                      vm_base + elf, (uint32_t)size, context, ea, task,
                                      dispatch_cause, claim_qpc, ready_qpc);
#if defined(YZ_PERF_PROFILE) && defined(_WIN32)
        if (scan_profile_enabled) scan_profile_note_dispatch(&scan_profile, dispatched);
#endif
        if (!dispatched) {
            spu_taskset_release(ea, task); /* leave guest ready state for retry */
#if defined(YZ_PERF_PROFILE) && defined(_WIN32)
            if (scan_profile_enabled)
                scan_profile_resume(&scan_profile, prep_qpc, prep_cycles,
                                    prep_qpc_valid, prep_cycles_valid);
#endif
            continue;
        }
        if (ps3_log_verbose())
            fprintf(stderr, "[taskset] scheduled task %u of 0x%08X elf=0x%08X\n", task, ea, elf);
#if defined(YZ_PERF_PROFILE) && defined(_WIN32)
        if (scan_profile_enabled)
            scan_profile_resume(&scan_profile, prep_qpc, prep_cycles,
                                prep_qpc_valid, prep_cycles_valid);
#endif
    }
#if defined(YZ_PERF_PROFILE) && defined(_WIN32)
    if (scan_profile_enabled) scan_profile_finish(&scan_profile);
#endif
}

void spu_taskset_start_ready(uint32_t ea)
{
    spu_taskset_start_ready_cause(ea, SPU_TASKSET_DISPATCH_UNKNOWN);
}

void spu_taskset_task_exited(uint32_t ea, uint32_t task)
{
    if (!ea || task >= 128) return;
    spu_lockline_lock();
    uint8_t* ts = vm_base + ea;
    uint8_t mask = (uint8_t)(0x80u >> (task & 7));
    const unsigned fields[] = {CSTS_RUNNING, CSTS_READY, CSTS_PENDING_READY, CSTS_ENABLED};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
        ts[fields[i] + task / 8] &= (uint8_t)~mask;
    uint8_t* run = ts_running(ea);
    if (run) run[task / 8] &= (uint8_t)~mask;
    spu_coh_notify_write(ea);
    spu_lockline_unlock();
    spu_taskset_start_ready_cause(ea, SPU_TASKSET_DISPATCH_TASK_EXIT);
}

void spu_taskset_poll_cause(uint32_t spurs_ea, uint32_t dispatch_cause)
{
    uint32_t tasksets[TS_TRACK];
    unsigned count = 0;
    spu_lockline_lock();
    for (unsigned i = 0; i < TS_TRACK; ++i) {
        uint32_t ea = s_ts_run[i].ea;
        if (ea && be32(vm_base + ea + CSTS_SPURS + 4) == spurs_ea)
            tasksets[count++] = ea;
    }
    spu_lockline_unlock();
    for (unsigned i = 0; i < count; ++i)
        spu_taskset_start_ready_cause(tasksets[i], dispatch_cause);
}

void spu_taskset_poll(uint32_t spurs_ea)
{
    spu_taskset_poll_cause(spurs_ea, SPU_TASKSET_DISPATCH_POLL);
}
