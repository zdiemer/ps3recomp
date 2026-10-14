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
#ifdef _WIN32
    /* Register once per dispatch pass. Notifications and this snapshot share
     * the lock, so a readiness edge cannot be lost between priming and scan. */
    if (s_spurs_wake_active) {
        spu_lockline_lock();
        uint8_t* initial_ts = vm_base + ea;
        uint32_t initial_owner = be32(initial_ts + CSTS_SPURS + 4);
        void* initial_event = taskset_wake_event_locked(initial_owner);
        if (initial_event)
            (void)spu_coh_watch_taskset(ea, initial_owner, initial_ts, initial_event,
                                        spu_workload_async_trace_mark);
        spu_lockline_unlock();
    }
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
        size_t size = spu_elf_image_size(vm_base + elf, 2u * 1024 * 1024);
        if (!size || !spu_workload_dispatch_async_task_provenance(
                         vm_base + elf, (uint32_t)size, context, ea, task,
                         dispatch_cause, claim_qpc, ready_qpc)) {
            spu_taskset_release(ea, task); /* leave guest ready state for retry */
            continue;
        }
        if (ps3_log_verbose())
            fprintf(stderr, "[taskset] scheduled task %u of 0x%08X elf=0x%08X\n", task, ea, elf);
    }
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
