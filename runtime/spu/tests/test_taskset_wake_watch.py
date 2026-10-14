"""Compile the exact taskset-wake watcher block with minimal Win32 stubs."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
COHERENCY = ROOT / "runtime" / "spu" / "spu_coherency.c"
BEGIN = "/* BEGIN SPU TASKSET WAKE WATCH */"
END = "/* END SPU TASKSET WAKE WATCH */"


def extracted_source() -> str:
    source = COHERENCY.read_text(encoding="utf-8")
    if source.count(BEGIN) != 1 or source.count(END) != 1:
        raise AssertionError("expected one taskset wake source block")
    block = source.split(BEGIN, 1)[1].split(END, 1)[0]
    # Verify both real coherence notification entry points route through this
    # tested predicate block, without copying their implementation into fixture.
    for name in ("spu_coh_notify_write", "spu_coh_notify_write_except"):
        tail = source.split("void " + name + "(", 1)[1].split("\n}", 1)[0]
        if "taskset_notify_publication(ea);" not in tail:
            raise AssertionError(f"{name} no longer notifies taskset watches")
    return block


HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
static unsigned qpc_calls;
static int trace_enabled = 1;
static uint64_t spu_workload_async_trace_mark(void)
{ return trace_enabled ? 1000 + ++qpc_calls : 0; }
static void assert_signaled(HANDLE event)
{ assert(WaitForSingleObject(event, 0) == WAIT_OBJECT_0); }
static void assert_not_signaled(HANDLE event)
{ assert(WaitForSingleObject(event, 0) == WAIT_TIMEOUT); }
@@WATCH_BLOCK@@

typedef struct { HANDLE event, entered; DWORD result; } waiter_args;
static DWORD WINAPI blocked_waiter(void* p)
{
    waiter_args* a = (waiter_args*)p;
    SetEvent(a->entered);
    a->result = WaitForSingleObject(a->event, INFINITE);
    return 0;
}

int main(void)
{
    uint8_t h[128] = {0}, h2[128] = {0};
    const uint32_t ea = 0x1080, owner = 0x4000;
    HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE event2 = CreateEventA(NULL, FALSE, FALSE, NULL);
    assert(event && event2);
    assert(!spu_coh_watch_taskset(ea + 1, owner, h, event, spu_workload_async_trace_mark)); /* header aligned */
    assert(!spu_coh_watch_taskset(ea, owner, h, NULL, spu_workload_async_trace_mark));

    /* With no diagnostic callback the wake still works and no QPC is returned. */
    trace_enabled = 0;
    assert(spu_coh_watch_taskset(ea, owner, h, event, spu_workload_async_trace_mark));
    h[SPU_TASKSET_ENABLED_OFF] = h[SPU_TASKSET_READY_OFF] = 0x80;
    taskset_notify_publication(ea);
    assert_signaled(event); /* signal set before wait is not lost */
    assert(qpc_calls == 0);
    assert(spu_coh_taskset_ready_qpc_locked(ea, 0) == 0);
    spu_coh_unwatch_tasksets(owner);
    memset(h, 0, sizeof(h));
    qpc_calls = 0;
    trace_enabled = 1;
    assert(spu_coh_watch_taskset(ea, owner, h, event, spu_workload_async_trace_mark));

    /* Task 0 is bit 0x80 in byte 0 (SPURS bitset is MSB-first). */
    h[SPU_TASKSET_ENABLED_OFF] = 0x80;
    h[SPU_TASKSET_READY_OFF] = 0x80;
    taskset_notify_publication(ea);
    assert_signaled(event);
    assert(qpc_calls == 1);
    assert(spu_coh_taskset_ready_qpc_locked(ea, 0) == 1001);
    assert(spu_coh_taskset_ready_qpc_locked(ea, 7) == 0);

    /* A second rising bit in that byte maps to task 7, sharing one edge QPC. */
    h[SPU_TASKSET_ENABLED_OFF] |= 0x01;
    h[SPU_TASKSET_PENDING_READY_OFF] = 0x01;
    taskset_notify_publication(ea);
    assert_signaled(event);
    assert(qpc_calls == 2);
    assert(spu_coh_taskset_ready_qpc_locked(ea, 7) == 1002);
    assert(spu_coh_taskset_ready_qpc_locked(ea, 0) == 1001);

    /* Repeated stores, unrelated lines, and unchanged eligible bits do not wake. */
    taskset_notify_publication(ea);
    taskset_notify_publication(ea + 0x2000);
    assert_not_signaled(event);
    assert(qpc_calls == 2);

    /* First bit in the second byte is task 8; pending-ready is also eligible. */
    h[SPU_TASKSET_ENABLED_OFF + 1] = 0x80;
    h[SPU_TASKSET_PENDING_READY_OFF + 1] = 0x80;
    taskset_notify_publication(ea);
    assert_signaled(event);
    assert(qpc_calls == 3);
    assert(spu_coh_taskset_ready_qpc_locked(ea, 8) == 1003);

    /* Last task uses the LSB of byte 15; indexing remains bounded at 127. */
    h[SPU_TASKSET_ENABLED_OFF + 15] = 0x01;
    h[SPU_TASKSET_READY_OFF + 15] = 0x01;
    taskset_notify_publication(ea);
    assert_signaled(event);
    assert(qpc_calls == 4);
    assert(spu_coh_taskset_ready_qpc_locked(ea, 127) == 1004);

    /* Both a ready-bit fall and an enabled-bit fall clear observed timestamps. */
    h[SPU_TASKSET_READY_OFF] = 0;
    taskset_notify_publication(ea);
    assert(spu_coh_taskset_ready_qpc_locked(ea, 0) == 0);
    h[SPU_TASKSET_ENABLED_OFF] &= (uint8_t)~0x01;
    taskset_notify_publication(ea);
    assert(spu_coh_taskset_ready_qpc_locked(ea, 7) == 0);
    assert_not_signaled(event);
    assert(qpc_calls == 4);

    /* New eligible state after a fall is a fresh edge, with a fresh timestamp. */
    h[SPU_TASKSET_ENABLED_OFF] |= 0x80;
    h[SPU_TASKSET_READY_OFF] |= 0x80;
    taskset_notify_publication(ea);
    assert_signaled(event);
    assert(qpc_calls == 5);
    assert(spu_coh_taskset_ready_qpc_locked(ea, 0) == 1005);

    /* Explicit SPURS reinit clears snapshots even when EA/host/event are reused. */
    spu_coh_unwatch_tasksets(owner);
    assert(!spu_coh_taskset_ready_qpc_locked(ea, 0));
    assert(spu_coh_watch_taskset(ea, owner, h, event, spu_workload_async_trace_mark));
    taskset_notify_publication(ea); /* currently eligible state is primed */
    assert_not_signaled(event);
    assert(qpc_calls == 5);

    /* A hash collision is rejected safely and keeps its own 1ms poll fallback. */
    uint32_t collision = 0;
    for (uint32_t x = ea + 128; x < ea + 0x200000; x += 128)
        if (taskset_watch_index(x) == taskset_watch_index(ea)) { collision = x; break; }
    assert(collision && !spu_coh_watch_taskset(collision, owner, h2,
                                               event2, spu_workload_async_trace_mark));
    spu_coh_unwatch_tasksets(owner);
    assert(!spu_coh_taskset_ready_qpc_locked(ea, 0));
    assert(spu_coh_watch_taskset(collision, owner, h2, event2,
                                 spu_workload_async_trace_mark));

    /* Signal while a thread is blocked. The ready state remains authoritative;
     * this event merely causes the kernel poller to scan sooner. */
    waiter_args wa = {event2, CreateEventA(NULL, TRUE, FALSE, NULL), WAIT_FAILED};
    assert(wa.entered);
    HANDLE thread = CreateThread(NULL, 0, blocked_waiter, &wa, 0, NULL);
    assert(thread && WaitForSingleObject(wa.entered, 1000) == WAIT_OBJECT_0);
    Sleep(10); /* let the consumer enter its auto-reset wait */
    h2[SPU_TASKSET_ENABLED_OFF] = h2[SPU_TASKSET_READY_OFF] = 0x80;
    taskset_notify_publication(collision);
    assert(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0);
    assert(wa.result == WAIT_OBJECT_0 && qpc_calls == 6);
    CloseHandle(thread); CloseHandle(wa.entered);
    CloseHandle(event); CloseHandle(event2);
    puts("Taskset wake watcher contract passed");
    return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.environ.get("CC", "clang-cl"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="spu-taskset-wake-") as td:
        td = Path(td)
        src, exe, obj = td / "fixture.c", td / "fixture.exe", td / "fixture.obj"
        src.write_text(HARNESS.replace("@@WATCH_BLOCK@@", extracted_source()), encoding="utf-8")
        cmd = [args.compiler, "/nologo", "/TC", "/std:c11", "/W4", "/WX",
               str(src), f"/Fe:{exe}", f"/Fo:{obj}"]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
