"""Exercise actual WAIT_SIGNAL and atomic channel source with Win32 threads.

Only unrelated channel diagnostics/coherence consumers are stubbed. The real
atomic compare/copy/notification code, guest bit accessors, signal mutex,
condition variable, parked registry and timeout loop execute in the fixture.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def between(source: str, begin: str, end: str) -> str:
    assert source.count(begin) == 1, begin
    return begin + source.split(begin, 1)[1].split(end, 1)[0]


def harness() -> str:
    channels = (ROOT / "runtime/spu/spu_channels.c").read_text(encoding="utf-8")
    workload = (ROOT / "runtime/spu/spu_workload.c").read_text(encoding="utf-8")
    atomic = between(channels, "static int spu_mfc_atomic(", "static int channel_is_mfc(")
    signals = between(workload, '#include "../../libs/spurs/spurs_taskset.h"',
                      "int spu_workload_dispatch_async_task_provenance(")
    signals = signals.replace('#include "../../libs/spurs/spurs_taskset.h"',
                              '#include "libs/spurs/spurs_taskset.h"')
    spurs = (ROOT / "libs/spurs/cellSpurs.c").read_text(encoding="utf-8")
    handoff = between(spurs, "            int woke = spu_taskset_signal_parked_obj(",
                      "            static int _n = 0;")
    return (HARNESS.replace("@@SIGNALS@@", signals).replace("@@ATOMIC@@", atomic)
            .replace("@@EVENTFLAG_HANDOFF@@", handoff))


HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime/spu/spu_context.h"
static uint8_t guest[1u << 20];
uint8_t* vm_base = guest;
static SRWLOCK coherence_lock = SRWLOCK_INIT;
static _Thread_local int coherence_held;
static volatile LONG broadcasts, commit_calls, flag_calls, ready_calls;
static HANDLE wait_entered, signal_lock_entered;
static HANDLE serial_acquire_entered, serial_acquire_go;
static volatile LONG pause_serial_acquire;
static uint64_t now_ms(void) { return GetTickCount64(); }
unsigned long long ps3_ms_now(void) { return now_ms(); }
uint32_t g_barrier_sync_watch;
static volatile unsigned g_spu_putllc_count, g_spu_putllc_sync_hit;
static void spu_serial_release(void) {}
static void spu_serial_acquire(void) {
    if (InterlockedCompareExchange(&pause_serial_acquire, 0, 1) == 1) {
        SetEvent(serial_acquire_entered);
        assert(WaitForSingleObject(serial_acquire_go, 2000) == WAIT_OBJECT_0);
    }
}
void spu_lockline_lock(void) {
    assert(!coherence_held);
    AcquireSRWLockExclusive(&coherence_lock); coherence_held = 1;
}
void spu_lockline_unlock(void) {
    assert(coherence_held); coherence_held = 0;
    ReleaseSRWLockExclusive(&coherence_lock);
}
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 1; }
void spu_coh_notify_write(uint32_t ea) {
    (void)ea; assert(coherence_held); InterlockedIncrement(&ready_calls);
}
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int g_resv_store_active;
uint32_t g_ww_lo = UINT32_MAX, g_ww_hi;
void ps3_ww_report_inline(uint32_t ea, uint64_t val, int width)
{ (void)ea; (void)val; (void)width; }
static void fixture_broadcast(PCONDITION_VARIABLE cv) {
    assert(!coherence_held); InterlockedIncrement(&broadcasts);
    WakeAllConditionVariable(cv);
}
static BOOL fixture_wait(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG flags) {
    assert(ms == 1000 && !coherence_held);
    SetEvent(wait_entered);
    return SleepConditionVariableSRW(cv, lock, ms, flags);
}
static void fixture_signal_lock(PSRWLOCK lock) {
    if (signal_lock_entered) SetEvent(signal_lock_entered);
    AcquireSRWLockExclusive(lock);
}
#define AcquireSRWLockExclusive fixture_signal_lock
#define WakeAllConditionVariable fixture_broadcast
#define SleepConditionVariableSRW fixture_wait
@@SIGNALS@@
#undef WakeAllConditionVariable
#undef SleepConditionVariableSRW
#undef AcquireSRWLockExclusive

static int mfc_ea_range_committed(uint32_t ea, uint32_t bytes)
{ return ea <= sizeof(guest) && bytes <= sizeof(guest) - ea; }
static void yz_lockstep_tick(spu_context* ctx) { (void)ctx; }
static void spu_coh_reserve(spu_context* ctx, uint32_t ea) { (void)ctx; (void)ea; }
void spu_lockguard_check(spu_context* ctx, uint32_t ea, const uint8_t* p,
                         uint32_t bytes, const char* what)
{ (void)ctx; (void)ea; (void)p; (void)bytes; (void)what; assert(coherence_held); }
static void fixture_commit(uint32_t line) {
    assert(!coherence_held); InterlockedIncrement(&commit_calls);
    spu_taskset_line_committed(line);
}
static void fixture_flag(uint32_t line)
{ (void)line; assert(!coherence_held); InterlockedIncrement(&flag_calls); }
void (*g_spu_line_commit_hook)(uint32_t) = fixture_flag;
#define spu_taskset_line_committed fixture_commit
@@ATOMIC@@
#undef spu_taskset_line_committed

#define TS_A 0x30000u
#define TS_B 0x32000u
#define TASK 3u
#define OTHER 5u
#define LS 0x1000u
static spu_context spu;
typedef struct { uint32_t ts, task; volatile LONG returned; } waiter;
static DWORD WINAPI wait_main(void* p) {
    waiter* w = p;
    spu_taskset_parked_add(w->ts, w->task, 0);
    spu_taskset_wait_signal(w->ts, w->task);
    spu_taskset_parked_del(w->ts, w->task);
    InterlockedExchange(&w->returned, 1);
    return 0;
}
static HANDLE park(waiter* w, uint32_t ts, uint32_t task) {
    *w = (waiter){ts, task, 0}; ResetEvent(wait_entered);
    HANDLE th = CreateThread(NULL, 0, wait_main, w, 0, NULL);
    assert(th && WaitForSingleObject(wait_entered, 2000) == WAIT_OBJECT_0);
    return th;
}
static void joined(HANDLE th, DWORD timeout) {
    assert(WaitForSingleObject(th, timeout) == WAIT_OBJECT_0);
    CloseHandle(th);
}
static void reset(void) {
    assert(!atomic_load(&s_parked_count));
    memset(guest, 0, sizeof(guest));
    broadcasts = commit_calls = flag_calls = ready_calls = 0;
    memset(&spu, 0, sizeof(spu)); spu.ls = spu.ls_store;
}
static void atomic_cmd(uint32_t ts, uint32_t cmd) {
    spu.mfc_eal = ts; spu.mfc_lsa = LS;
    assert(spu_mfc_atomic(&spu, cmd) == 1);
}
static void prepare_signal(uint32_t ts, uint32_t task) {
    atomic_cmd(ts, MFC_GETLLAR_CMD);
    spu.ls[LS + CSTS_SIGNALLED + task / 8] |= (uint8_t)(0x80u >> (task & 7));
}
static void signal_spu(uint32_t ts, uint32_t task, uint32_t cmd) {
    prepare_signal(ts, task); atomic_cmd(ts, cmd);
}
static int signalled(uint32_t ts, uint32_t task)
{ return spurs_bitset_test(ts + CSTS_SIGNALLED, task); }

static void during_wait(uint32_t cmd) {
    reset(); waiter w; HANDLE th = park(&w, TS_A, TASK);
    signal_spu(TS_A, TASK, cmd);
    assert(spu.atomic_stat == (cmd == MFC_PUTLLC_CMD ? 0u : 2u));
    joined(th, 400);
    assert(w.returned && !signalled(TS_A, TASK));
    assert(commit_calls == 1 && flag_calls == 1 && broadcasts >= 1);
    assert(ready_calls >= 1 && atomic_load(&s_parked_count) == 0);
}
static void before_wait(void) {
    reset(); signal_spu(TS_A, TASK, MFC_PUTLLC_CMD);
    assert(broadcasts == 0 && signalled(TS_A, TASK));
    waiter w = {TS_A, TASK, 0};
    HANDLE th = CreateThread(NULL, 0, wait_main, &w, 0, NULL);
    assert(th); joined(th, 400);
    assert(!signalled(TS_A, TASK) && !atomic_load(&s_parked_count));
    /* Publication after registration but before checking the predicate. */
    spu_taskset_parked_add(TS_A, TASK, 0);
    signal_spu(TS_A, TASK, MFC_PUTLLUC_CMD);
    assert(broadcasts == 1);
    assert(spu_taskset_wait_signal(TS_A, TASK) == 0);
    spu_taskset_parked_del(TS_A, TASK);
    assert(!signalled(TS_A, TASK) && !atomic_load(&s_parked_count));
}
static void spurious_and_failed(void) {
    reset(); waiter w; HANDLE th = park(&w, TS_A, TASK);
    signal_spu(TS_A, OTHER, MFC_PUTLLC_CMD);
    assert(broadcasts == 1 && WaitForSingleObject(th, 80) == WAIT_TIMEOUT);
    signal_spu(TS_B, TASK, MFC_PUTLLUC_CMD);
    assert(broadcasts == 1 && WaitForSingleObject(th, 80) == WAIT_TIMEOUT);
    /* Same-line commit without any new signal is also harmless. */
    atomic_cmd(TS_A, MFC_GETLLAR_CMD); atomic_cmd(TS_A, MFC_PUTLLUC_CMD);
    assert(broadcasts == 2 && WaitForSingleObject(th, 80) == WAIT_TIMEOUT);
    LONG commits = commit_calls, flags = flag_calls, ready = ready_calls;
    prepare_signal(TS_A, TASK); spu.resv_valid = 0;
    atomic_cmd(TS_A, MFC_PUTLLC_CMD);
    assert(spu.atomic_stat == 1 && !signalled(TS_A, TASK));
    assert(commits == commit_calls && flags == flag_calls && ready == ready_calls);
    assert(broadcasts == 2 && WaitForSingleObject(th, 80) == WAIT_TIMEOUT);
    /* A mismatching reservation snapshot must not notify either. */
    prepare_signal(TS_A, TASK);
    spu_lockline_lock(); guest[TS_A + 0x70] ^= 1; spu_lockline_unlock();
    atomic_cmd(TS_A, MFC_PUTLLC_CMD);
    assert(spu.atomic_stat == 1 && commits == commit_calls && flags == flag_calls);
    assert(WaitForSingleObject(th, 80) == WAIT_TIMEOUT);
    signal_spu(TS_A, TASK, MFC_PUTLLC_CMD); joined(th, 400);
}
static void reuse(void) {
    reset();
    spu_taskset_parked_add(TS_A, TASK, 0x1110);
    spu_taskset_parked_add(TS_A, TASK, 0x2220);
    assert(atomic_load(&s_parked_count) == 1);
    assert(spu_taskset_signal_parked_obj(TS_A, 0x1110) == 0);
    spu_taskset_parked_del(TS_A, TASK);
    spu_taskset_parked_add(TS_A, OTHER, 0x3330);
    assert(spu_taskset_signal_parked_obj(TS_A, 0x2220) == 0);
    assert(!signalled(TS_A, TASK) && !signalled(TS_A, OTHER));
    assert(spu_taskset_signal_parked_obj(TS_A, 0x3330) == 1);
    assert(spu_taskset_wait_signal(TS_A, OTHER) == 0);
    spu_taskset_parked_del(TS_A, OTHER);
    assert(!atomic_load(&s_parked_count) && !signalled(TS_A, OTHER));
    /* Reusing the same task id cannot consume a signal from the completed wait. */
    waiter w; HANDLE th = park(&w, TS_A, OTHER);
    assert(WaitForSingleObject(th, 80) == WAIT_TIMEOUT);
    signal_spu(TS_A, OTHER, MFC_PUTLLUC_CMD); joined(th, 400);
    for (unsigned i = 0; i < SPU_PARKED_MAX; ++i)
        assert(!s_parked[i] && !s_parked_obj[i]);
}
static DWORD WINAPI commit_main(void* p) {
    (void)p; atomic_cmd(TS_A, MFC_PUTLLC_CMD); return 0;
}
static void last_deletion_overlap(void) {
    reset(); spu_taskset_parked_add(TS_A, TASK, 0);
    prepare_signal(TS_A, TASK);
    taskset_signal_lock(); ResetEvent(signal_lock_entered);
    HANDLE th = CreateThread(NULL, 0, commit_main, NULL, 0, NULL); assert(th);
    /* The notifier saw a nonempty table and is now blocked taking its mutex.
     * Model the caller's locked registry cleanup before it acquires that mutex. */
    assert(WaitForSingleObject(signal_lock_entered, 2000) == WAIT_OBJECT_0);
    assert(commit_calls == 1);
    taskset_parked_del_locked(TS_A, TASK);
    assert(!atomic_load(&s_parked_count)); taskset_signal_unlock();
    joined(th, 400);
    assert(broadcasts == 0 && flag_calls == 1 && ready_calls == 1);
}
static int eventflag_handoff(uint32_t taskset_ea, uint32_t ea) {
    /* The actual opt-in EventFlagSet lookup/fallback/latch source. */
@@EVENTFLAG_HANDOFF@@
    return woke;
}
static void completed_wait_eventflag_signal(void) {
    reset(); ResetEvent(serial_acquire_entered); ResetEvent(serial_acquire_go);
    InterlockedExchange(&pause_serial_acquire, 1);
    waiter w; HANDLE th = park(&w, TS_A, TASK);
    signal_spu(TS_A, TASK, MFC_PUTLLC_CMD);
    assert(WaitForSingleObject(serial_acquire_entered, 2000) == WAIT_OBJECT_0);
    assert(!signalled(TS_A, TASK)); /* first wait consumed its signal */
    /* Preserve the f15dfce registration lifetime. Removing the entry inside
     * WAIT_SIGNAL instead returns zero here and takes the unsynchronized
     * taskset-latch handoff. A next consume/park could then beat latch_wake. */
    assert(eventflag_handoff(TS_A, 0x1080) == 1);
    assert(signalled(TS_A, TASK) && !spu_taskset_consume_wake(TS_A));
    SetEvent(serial_acquire_go); joined(th, 400);
    assert(!atomic_load(&s_parked_count));
    /* The next logical wait consumes the second guest signal immediately. */
    w = (waiter){TS_A, TASK, 0};
    th = CreateThread(NULL, 0, wait_main, &w, 0, NULL); assert(th);
    joined(th, 400);
    assert(!signalled(TS_A, TASK) && !atomic_load(&s_parked_count));
}
static HANDLE registry_go;
static DWORD WINAPI registry_thread(void* p) {
    uint32_t task = (uint32_t)(uintptr_t)p;
    assert(WaitForSingleObject(registry_go, 2000) == WAIT_OBJECT_0);
    spu_taskset_parked_add(TS_A, task, 0x1000 + task);
    return 0;
}
static void concurrent_registry(void) {
    reset(); HANDLE threads[48];
    registry_go = CreateEventA(NULL, TRUE, FALSE, NULL); assert(registry_go);
    for (unsigned i = 0; i < 48; ++i) {
        threads[i] = CreateThread(NULL, 0, registry_thread, (void*)(uintptr_t)i, 0, NULL);
        assert(threads[i]);
    }
    SetEvent(registry_go);
    for (unsigned i = 0; i < 48; ++i) joined(threads[i], 2000);
    assert(atomic_load(&s_parked_count) == 48);
    for (unsigned i = 0; i < 48; ++i) {
        assert(spu_taskset_signal_parked_obj(TS_A, 0x1000 + i) == 1);
        assert(signalled(TS_A, i)); spu_taskset_parked_del(TS_A, i);
    }
    CloseHandle(registry_go); assert(!atomic_load(&s_parked_count));
}
static void fallback(void) {
    reset();
    for (unsigned i = 0; i < SPU_PARKED_MAX; ++i)
        spu_taskset_parked_add(TS_B, i, 0);
    waiter w; HANDLE th = park(&w, TS_A, TASK);
    assert(atomic_load(&s_parked_count) == SPU_PARKED_MAX);
    signal_spu(TS_A, TASK, MFC_PUTLLC_CMD);
    assert(broadcasts == 0 && WaitForSingleObject(th, 100) == WAIT_TIMEOUT);
    joined(th, 1500); /* full registry retains the actual one-second poll */
    assert(!signalled(TS_A, TASK));
    for (unsigned i = 0; i < SPU_PARKED_MAX; ++i) spu_taskset_parked_del(TS_B, i);
    assert(!atomic_load(&s_parked_count));
    reset(); th = park(&w, TS_A, TASK);
    assert(WaitForSingleObject(th, 1100) == WAIT_TIMEOUT); /* no invented signal */
    spu_taskset_signal_task(TS_A, TASK); joined(th, 400);
}
int main(void) {
    SetEnvironmentVariableA("LBP_WS_DRAIN", NULL);
    SetEnvironmentVariableA("LBP_SYNC_ACK", NULL);
    wait_entered = CreateEventA(NULL, TRUE, FALSE, NULL); assert(wait_entered);
    signal_lock_entered = CreateEventA(NULL, TRUE, FALSE, NULL); assert(signal_lock_entered);
    serial_acquire_entered = CreateEventA(NULL, TRUE, FALSE, NULL); assert(serial_acquire_entered);
    serial_acquire_go = CreateEventA(NULL, TRUE, FALSE, NULL); assert(serial_acquire_go);
    before_wait();
    during_wait(MFC_PUTLLC_CMD); during_wait(MFC_PUTLLUC_CMD);
    during_wait(MFC_PUTQLLUC_CMD);
    spurious_and_failed(); reuse(); last_deletion_overlap(); completed_wait_eventflag_signal();
    concurrent_registry(); fallback();
    CloseHandle(wait_entered); CloseHandle(signal_lock_entered);
    CloseHandle(serial_acquire_entered); CloseHandle(serial_acquire_go);
    puts("SPU signal wake actual-source contract passed"); return 0;
}
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.environ.get("CC", "clang-cl"))
    parser.add_argument("--emit-only", type=Path, help="Write the fixture without compiling")
    args = parser.parse_args()
    source = harness()
    if args.emit_only:
        args.emit_only.write_text(source, encoding="utf-8")
        return 0
    with tempfile.TemporaryDirectory(prefix="spu-signal-wake-") as td:
        td = Path(td)
        src, exe, obj = td / "fixture.c", td / "fixture.exe", td / "fixture.obj"
        src.write_text(source, encoding="utf-8")
        subprocess.run([args.compiler, "/nologo", "/TC", "/std:c11", "/W4",
                        f"/I{ROOT}", str(src), f"/Fe:{exe}", f"/Fo:{obj}"], check=True)
        subprocess.run([str(exe)], check=True, timeout=20)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
