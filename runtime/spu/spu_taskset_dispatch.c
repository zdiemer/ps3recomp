/* Schedule guest-created tasks while persistent controller tasks are alive.
 * Metadata and reservations share the SPU lock-line lock, so host claims cannot
 * race a guest PUTLLC. Image lookup and host thread creation happen unlocked. */
#include "spu_workload.h"
#include "spu_coherency.h"
#include "../../libs/spurs/spurs_taskset.h"
#include <stdio.h>
#include <string.h>

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

static void ts_schedule(uint32_t ea)
{
    for (uint32_t task = 0; task < 128; ++task) {
        uint8_t mask = (uint8_t)(0x80u >> (task & 7));
        uint32_t elf = 0, context = 0;
        spu_lockline_lock();
        uint8_t* ts = vm_base + ea;
        uint8_t* run = ts_running(ea);
        if (run && !(run[task / 8] & mask) &&
            (ts[CSTS_ENABLED + task / 8] & mask) &&
            ((ts[CSTS_READY + task / 8] | ts[CSTS_PENDING_READY + task / 8]) & mask)) {
            const uint8_t* ti = ts + CSTS_TASKINFO + 0x30 * task;
            elf = be32(ti + TI_ELF + 4) & ~7u;
            context = be32(ti + TI_CONTEXT + 4) & ~127u;
            if (elf) run[task / 8] |= mask; /* claim before unlocking/spawning */
        }
        spu_lockline_unlock();
        if (!elf) continue;
        size_t size = spu_elf_image_size(vm_base + elf, 2u * 1024 * 1024);
        if (!size || !spu_workload_dispatch_async_task(vm_base + elf, (uint32_t)size,
                                                      context, ea, task)) {
            spu_taskset_release(ea, task); /* leave guest ready state for retry */
            continue;
        }
        fprintf(stderr, "[taskset] scheduled task %u of 0x%08X elf=0x%08X\n", task, ea, elf);
    }
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
    ts_schedule(ea);
}

void spu_taskset_poll(uint32_t spurs_ea)
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
    for (unsigned i = 0; i < count; ++i) ts_schedule(tasksets[i]);
}
