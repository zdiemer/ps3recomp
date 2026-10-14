"""Exercise production wake policy, readiness watcher and dispatcher together."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_taskset_wake_watch import ROOT, extracted_source


def unique_between(source: str, begin: str, end: str) -> str:
    assert source.count(begin) == 1, begin
    tail = source.split(begin, 1)[1]
    assert end in tail, end
    return tail.split(end, 1)[0]


def harness() -> str:
    source = (ROOT / 'libs/spurs/cellSpurs.c').read_text(encoding='utf-8')
    mode = unique_between(source, 'static volatile LONG s_taskset_wake_mode = -1;',
                          'static struct SpursInst* spurs_inst_find')
    configure = '    if (!si->taskset_wake_configured)' + unique_between(
        source, '    if (!si->taskset_wake_configured)', '    /* Real BE instance:')
    wait = '        uint32_t poll_cause = SPU_TASKSET_DISPATCH_POLL;' + unique_between(
        source, '        uint32_t poll_cause = SPU_TASKSET_DISPATCH_POLL;', '        u32 ea = si->ea;')
    return (HARNESS.replace('@@DISPATCH@@', (ROOT / 'runtime/spu/spu_taskset_dispatch.c').as_posix())
            .replace('@@WATCH@@', extracted_source()).replace('@@MODE@@', mode)
            .replace('@@CONFIGURE@@', configure).replace('@@WAIT@@', wait))


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "@@DISPATCH@@"
@@WATCH@@

static uint8_t memory[0x10000];
uint8_t* vm_base = memory;
static int locked, fail_dispatch, dispatches;
static uint64_t clock_value, last_ready, last_claim;
static uint32_t last_cause;
void spu_lockline_lock(void) { assert(!locked); locked = 1; }
void spu_lockline_unlock(void) { assert(locked); locked = 0; }
void spu_coh_notify_write(uint32_t ea)
{ assert(locked); taskset_notify_publication(ea); }
uint64_t spu_workload_async_trace_mark(void) { return ++clock_value; }
size_t spu_elf_image_size(const uint8_t* p, size_t max)
{ assert(!locked && p == memory + 0x4000 && max >= 16); return 16; }
int spu_workload_dispatch_async_task_provenance(const uint8_t* p, uint32_t size,
    uint32_t context, uint32_t ts, uint32_t task, uint32_t cause,
    uint64_t claim_qpc, uint64_t ready_qpc)
{
    assert(!locked && p == memory + 0x4000 && size == 16);
    assert(context == 0x5000 && ts == 0x100 && task < 128);
    assert(!ready_qpc || ready_qpc < claim_qpc);
    ++dispatches; last_cause = cause; last_ready = ready_qpc; last_claim = claim_qpc;
    return !fail_dispatch;
}
static void be_write(uint32_t ea, uint32_t value)
{
    for (unsigned b = 0; b < 4; ++b) memory[ea + b] = (uint8_t)(value >> (24 - 8*b));
}
static void publish(unsigned task)
{
    spu_lockline_lock();
    const uint8_t bit = (uint8_t)(0x80u >> (task & 7));
    uint32_t ti = 0x100 + CSTS_TASKINFO + task * TI_SIZE;
    be_write(ti + TI_ELF + 4, 0x4000);
    be_write(ti + TI_CONTEXT + 4, 0x5000);
    memory[0x100 + CSTS_ENABLED + task / 8] |= bit;
    memory[0x100 + CSTS_PENDING_READY + task / 8] |= bit;
    spu_coh_notify_write(0x100);
    spu_lockline_unlock();
}
static uint64_t readiness(unsigned task)
{
    spu_lockline_lock();
    uint64_t value = spu_coh_taskset_ready_qpc_locked(0x100, task);
    spu_lockline_unlock();
    return value;
}

/* The configuration/wait blocks below are extracted verbatim from cellSpurs.
 * Only Win32 failure injection and the scheduler's other fields are fixtures. */
struct SpursInst {
    HANDLE taskset_wake_event;
    int taskset_wake_enabled, taskset_wake_configured;
};
static volatile LONG s_taskset_wake_mode = -1;
@@MODE@@
static int fail_event, fail_wait, create_calls, sleep_calls;
static HANDLE create_event(LPSECURITY_ATTRIBUTES a, BOOL manual, BOOL initial, LPCSTR name)
{
    ++create_calls;
    return fail_event ? NULL : CreateEventA(a, manual, initial, name);
}
static DWORD wait_event(HANDLE h, DWORD timeout)
{ assert(timeout == 1); return fail_wait ? WAIT_FAILED : WaitForSingleObject(h, timeout); }
static void fallback_sleep(DWORD ms) { assert(ms == 1); ++sleep_calls; }
#define CreateEventA create_event
static void configure(struct SpursInst* si, uint32_t spurs_ea) {
@@CONFIGURE@@
}
#undef CreateEventA
#define WaitForSingleObject wait_event
#define Sleep fallback_sleep
static uint32_t wait_cause(struct SpursInst* si) {
@@WAIT@@
    return poll_cause;
}
#undef WaitForSingleObject
#undef Sleep

static void mode(const char* value)
{
    assert(_putenv_s("SPU_TASKSET_WAKE", value) == 0);
    InterlockedExchange(&s_taskset_wake_mode, -1);
}
int main(void)
{
    const uint32_t owner = 0x2000;
    be_write(0x100 + CSTS_SPURS + 4, owner);
    struct SpursInst disabled = {0}, failed = {0}, live = {0};
    mode("0"); configure(&disabled, owner);
    assert(!create_calls && !disabled.taskset_wake_enabled && !s_spurs_wake_active);
    assert(wait_cause(&disabled) == SPU_TASKSET_DISPATCH_POLL && sleep_calls == 1);
    mode("true"); assert(!spurs_taskset_wake_requested()); /* only exact 1 opts in */
    mode("1"); fail_event = 1; configure(&failed, owner);
    assert(create_calls == 1 && !failed.taskset_wake_enabled && !s_spurs_wake_active);
    assert(wait_cause(&failed) == SPU_TASKSET_DISPATCH_POLL && sleep_calls == 2);
    fail_event = 0; configure(&live, owner);
    assert(create_calls == 2 && live.taskset_wake_enabled && s_spurs_wake_active);
    assert(wait_cause(&live) == SPU_TASKSET_DISPATCH_POLL); /* bounded real timeout */
    assert(sleep_calls == 2);
    fail_wait = 1;
    assert(wait_cause(&live) == SPU_TASKSET_DISPATCH_POLL && sleep_calls == 3);
    fail_wait = 0;

    /* Register the real watcher while no task is ready. */
    spu_taskset_start_ready(0x100);
    assert(s_taskset_watch_count == 1 && !dispatches);
    SetEvent(live.taskset_wake_event); /* spurious wake must not invent work */
    spu_taskset_poll_cause(owner, wait_cause(&live));
    assert(!dispatches);

    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        unsigned task = iteration % 128;
        publish(task); /* publication before wait remains latched */
        assert(wait_cause(&live) == SPU_TASKSET_DISPATCH_WAKE);
        spu_taskset_poll_cause(owner, SPU_TASKSET_DISPATCH_WAKE);
        assert(dispatches == (int)iteration + 1 && last_ready && last_claim > last_ready);
        assert(last_cause == SPU_TASKSET_DISPATCH_WAKE);
        publish(task); /* unchanged readiness does not create another wake */
        assert(WaitForSingleObject(live.taskset_wake_event, 0) == WAIT_TIMEOUT);
        spu_taskset_poll(owner); /* host claim prevents duplicate dispatch */
        assert(dispatches == (int)iteration + 1);
        spu_taskset_task_exited(0x100, task); /* real exit clears ready snapshot */
        assert(!readiness(task));
    }
    publish(7); fail_dispatch = 1;
    assert(wait_cause(&live) == SPU_TASKSET_DISPATCH_WAKE);
    spu_taskset_poll_cause(owner, SPU_TASKSET_DISPATCH_WAKE);
    assert(dispatches == 1001);
    fail_dispatch = 0;
    assert(wait_cause(&live) == SPU_TASKSET_DISPATCH_POLL);
    spu_taskset_poll(owner); /* no second edge: fallback retries failed launch */
    assert(dispatches == 1002 && last_cause == SPU_TASKSET_DISPATCH_POLL);
    spu_taskset_task_exited(0x100, 7);

    /* Same address/handle reinitialization resets readiness history. */
    publish(8);
    assert(readiness(8));
    configure(&live, owner);
    assert(create_calls == 2 && s_taskset_watch_count == 0);
    assert(!readiness(8));
    spu_taskset_start_ready(0x100); /* already-ready work must still dispatch */
    assert(dispatches == 1003 && !last_ready);
    spu_taskset_task_exited(0x100, 8);
    (void)WaitForSingleObject(live.taskset_wake_event, 0); /* harmless old signal */

    spu_taskset_wake_unregister(owner);
    assert(!s_spurs_wake_active && !s_taskset_watch_count);
    publish(9);
    assert(WaitForSingleObject(live.taskset_wake_event, 0) == WAIT_TIMEOUT);
    spu_taskset_poll(owner); /* removed watch never suppresses ordinary readiness */
    assert(dispatches == 1004 && !last_ready);
    spu_taskset_task_exited(0x100, 9);
    configure(&live, owner); /* process-lifetime event reused, no new handle */
    assert(create_calls == 2 && s_spurs_wake_active);
    spu_taskset_start_ready(0x100);
    publish(127);
    assert(wait_cause(&live) == SPU_TASKSET_DISPATCH_WAKE);
    spu_taskset_poll_cause(owner, SPU_TASKSET_DISPATCH_WAKE);
    assert(dispatches == 1005 && last_ready);
    spu_taskset_task_exited(0x100, 127);
    spu_taskset_wake_unregister(owner);
    CloseHandle(live.taskset_wake_event);
    puts("Taskset wake integration passed: policy failures, 1000 recycled tasks, retry and reuse");
    return 0;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default=os.environ.get('CC', 'clang-cl'))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='spu-taskset-wake-integration-') as directory:
        root = Path(directory)
        src, exe, obj = root / 'fixture.c', root / 'fixture.exe', root / 'fixture.obj'
        src.write_text(harness(), encoding='utf-8')
        subprocess.run([args.compiler, '/nologo', '/TC', '/std:c17', '/W0', '/O2',
                        str(src), f'/Fe:{exe}', f'/Fo:{obj}'], check=True)
        subprocess.run([str(exe)], check=True, env={**os.environ, 'PS3_VERBOSE': '0'})


if __name__ == '__main__':
    main()
