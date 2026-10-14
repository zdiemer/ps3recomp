"""Compile the actual taskset scanner with opt-in scan profiling enabled."""
from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'spu_taskset_dispatch.c'

HARNESS = r'''
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int locked;
static volatile LONG64 fake_qpc, fake_cycles;
static unsigned qpc_calls, cycle_calls, lock_calls, unlock_calls, dispatches;
static const uint8_t *expected_image;
static HANDLE first_slot_busy, release_first_slot, dispatch_entered, release_dispatch;
static LONG block_dispatch, pause_first_publish, fail_qpc;
static void fixture_after_busy(void* slot, uint64_t ticket)
{
    (void)slot;
    if (ticket == 0 && InterlockedCompareExchange(&pause_first_publish, 0, 0)) {
        SetEvent(first_slot_busy);
        WaitForSingleObject(release_first_slot, INFINITE);
    }
}

static BOOL WINAPI fixture_qpc(LARGE_INTEGER *value)
{
    assert(!locked && value);
    ++qpc_calls;
    if (InterlockedCompareExchange(&fail_qpc, 0, 0)) return FALSE;
    value->QuadPart = InterlockedIncrement64(&fake_qpc);
    return TRUE;
}
static BOOL WINAPI fixture_frequency(LARGE_INTEGER *value)
{
    assert(!locked && value);
    value->QuadPart = 1000000;
    return TRUE;
}
static BOOL WINAPI fixture_cycles(HANDLE thread, PULONG64 value)
{
    assert(!locked && thread == GetCurrentThread() && value);
    ++cycle_calls;
    *value = (ULONG64)InterlockedIncrement64(&fake_cycles);
    return TRUE;
}
static DWORD WINAPI fixture_tid(void) { return 77; }
#define QueryPerformanceCounter fixture_qpc
#define QueryPerformanceFrequency fixture_frequency
#define QueryThreadCycleTime fixture_cycles
#define GetCurrentThreadId fixture_tid
#define SPU_TASKSET_SCAN_PROFILE_CAPACITY 4u
#define SPU_TASKSET_SCAN_PROFILE_DUMP_TIMEOUT_MS 50u
#define SPU_TASKSET_SCAN_PROFILE_TEST_HOOK_AFTER_BUSY(slot, ticket) fixture_after_busy((slot), (ticket))
#define YZ_PERF_PROFILE 1
#include "@@DISPATCH@@"

static uint8_t memory[0x10000];
uint8_t* vm_base = memory;
void spu_lockline_lock(void) { assert(!locked); locked = 1; ++lock_calls; }
void spu_lockline_unlock(void) { assert(locked); locked = 0; ++unlock_calls; }
void spu_coh_notify_write(uint32_t ea) { assert(locked); (void)ea; }
int spu_coh_watch_taskset(uint32_t ea, uint32_t owner, void* host, void* event,
                          uint64_t (*qpc)(void))
{ assert(locked); (void)ea; (void)owner; (void)host; (void)event; (void)qpc; return 0; }
void spu_coh_unwatch_tasksets(uint32_t owner) { assert(locked); (void)owner; }
uint64_t spu_coh_taskset_ready_qpc_locked(uint32_t ea, uint32_t task)
{ assert(locked); (void)ea; (void)task; return 0; }
size_t spu_elf_image_size(const uint8_t* p, size_t max)
{
    assert(!locked && p == expected_image && max == 2u * 1024u * 1024u);
    InterlockedExchangeAdd64(&fake_qpc, 10000);
    InterlockedExchangeAdd64(&fake_cycles, 20000);
    return 16;
}
uint64_t spu_workload_async_trace_mark(void)
{ assert(locked); return (uint64_t)InterlockedIncrement64(&fake_qpc) + 100000; }
int spu_workload_dispatch_async_task_provenance(const uint8_t* p, uint32_t size,
    uint32_t context, uint32_t ts, uint32_t task, uint32_t cause,
    uint64_t claim_qpc, uint64_t ready_qpc)
{
    assert(!locked && p == expected_image && size == 16 && context == 0x5000);
    assert(ts == 0x100 && task == 3 && cause == SPU_TASKSET_DISPATCH_POLL);
    assert(claim_qpc && !ready_qpc);
    ++dispatches;
    if (InterlockedCompareExchange(&block_dispatch, 0, 0)) {
        SetEvent(dispatch_entered);
        WaitForSingleObject(release_dispatch, INFINITE);
    }
    return 1;
}
static void be_write(uint32_t ea, uint32_t value)
{
    memory[ea] = (uint8_t)(value >> 24);
    memory[ea + 1] = (uint8_t)(value >> 16);
    memory[ea + 2] = (uint8_t)(value >> 8);
    memory[ea + 3] = (uint8_t)value;
}
static void make_ready(void)
{
    uint32_t ti = 0x100 + CSTS_TASKINFO + 3 * TI_SIZE;
    memory[0x100 + CSTS_ENABLED] |= 0x10;
    memory[0x100 + CSTS_READY] |= 0x10;
    be_write(ti + TI_ELF + 4, 0x4003);
    be_write(ti + TI_CONTEXT + 4, 0x507A);
}
static DWORD WINAPI empty_scan_thread(void* unused)
{
    (void)unused;
    spu_taskset_start_ready_cause(0x100, SPU_TASKSET_DISPATCH_POLL);
    return 0;
}
static DWORD WINAPI ready_scan_thread(void* unused)
{
    (void)unused;
    spu_taskset_start_ready_cause(0x100, SPU_TASKSET_DISPATCH_POLL);
    return 0;
}
static DWORD WINAPI release_dispatch_thread(void* unused)
{
    (void)unused;
    Sleep(5);
    SetEvent(release_dispatch);
    return 0;
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "";
    const char* alternate = argc > 2 ? argv[2] : "";
    expected_image = memory + 0x4000;
    if (!strcmp(mode, "disabled")) {
        assert(!getenv("SPU_TASKSET_SCAN_PROFILE_CSV"));
        spu_taskset_start_ready_cause(0x100, SPU_TASKSET_DISPATCH_POLL);
        assert(lock_calls == 128 && unlock_calls == 128 && dispatches == 0);
        spu_taskset_scan_profile_dump();
        assert(qpc_calls == 0 && cycle_calls == 0);
        puts("disabled scan performs no clock queries");
        return 0;
    }
    assert(getenv("SPU_TASKSET_SCAN_PROFILE_CSV"));
    if (!strcmp(mode, "qpcfail")) {
        InterlockedExchange(&fail_qpc, 1);
        spu_taskset_start_ready_cause(0x100, SPU_TASKSET_DISPATCH_POLL);
        spu_taskset_scan_profile_dump();
        puts("QPC setup failure leaves an incomplete header-only profile");
        return 0;
    } else if (!strcmp(mode, "wrap")) {
        for (unsigned i = 0; i < 6; ++i)
            spu_taskset_start_ready_cause(0x100, SPU_TASKSET_DISPATCH_POLL);
        assert(lock_calls == 6 * 128 && unlock_calls == 6 * 128);
    } else if (!strcmp(mode, "enabled")) {
        spu_taskset_start_ready_cause(0x100, SPU_TASKSET_DISPATCH_POLL); /* empty scan */
        assert(lock_calls == 128 && unlock_calls == 128 && dispatches == 0);
        assert(_putenv_s("SPU_TASKSET_SCAN_PROFILE_CSV", alternate) == 0);
        make_ready();
        spu_taskset_start_ready_cause(0x100, SPU_TASKSET_DISPATCH_POLL);
        assert(lock_calls == 256 && unlock_calls == 256 && dispatches == 1);
    } else if (!strcmp(mode, "ringrace")) {
        first_slot_busy = CreateEventA(NULL, TRUE, FALSE, NULL);
        release_first_slot = CreateEventA(NULL, TRUE, FALSE, NULL);
        assert(first_slot_busy && release_first_slot);
        InterlockedExchange(&pause_first_publish, 1);
        HANDLE first = CreateThread(NULL, 0, empty_scan_thread, NULL, 0, NULL);
        assert(first && WaitForSingleObject(first_slot_busy, 5000) == WAIT_OBJECT_0);
        for (unsigned i = 0; i < 4; ++i)
            spu_taskset_start_ready_cause(0x100, SPU_TASKSET_DISPATCH_POLL);
        SetEvent(release_first_slot);
        assert(WaitForSingleObject(first, 5000) == WAIT_OBJECT_0);
        CloseHandle(first);
        CloseHandle(first_slot_busy);
        CloseHandle(release_first_slot);
    } else if (!strcmp(mode, "repeatdump")) {
        spu_taskset_start_ready_cause(0x100, SPU_TASKSET_DISPATCH_POLL);
        spu_taskset_scan_profile_dump();
        FILE* file = fopen(getenv("SPU_TASKSET_SCAN_PROFILE_CSV"), "ab");
        assert(file);
        int sentinel_write = fputs("# fixture sentinel\n", file);
        int sentinel_close = fclose(file);
        assert(sentinel_write >= 0 && sentinel_close == 0);
        spu_taskset_scan_profile_dump();
        puts("scan profile dump remains frozen after the first export");
        return 0;
    } else if (!strcmp(mode, "timeout")) {
        dispatch_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
        release_dispatch = CreateEventA(NULL, TRUE, FALSE, NULL);
        assert(dispatch_entered && release_dispatch);
        make_ready();
        InterlockedExchange(&block_dispatch, 1);
        HANDLE worker = CreateThread(NULL, 0, ready_scan_thread, NULL, 0, NULL);
        assert(worker && WaitForSingleObject(dispatch_entered, 5000) == WAIT_OBJECT_0);
        spu_taskset_scan_profile_dump();
        SetEvent(release_dispatch);
        assert(WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0);
        CloseHandle(worker);
        CloseHandle(dispatch_entered);
        CloseHandle(release_dispatch);
        assert(!locked);
        puts("bounded profile dump reports an inflight writer without reading rows");
        return 0;
    } else if (!strcmp(mode, "drain")) {
        dispatch_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
        release_dispatch = CreateEventA(NULL, TRUE, FALSE, NULL);
        assert(dispatch_entered && release_dispatch);
        make_ready();
        InterlockedExchange(&block_dispatch, 1);
        HANDLE worker = CreateThread(NULL, 0, ready_scan_thread, NULL, 0, NULL);
        assert(worker && WaitForSingleObject(dispatch_entered, 5000) == WAIT_OBJECT_0);
        HANDLE releaser = CreateThread(NULL, 0, release_dispatch_thread, NULL, 0, NULL);
        assert(releaser);
        spu_taskset_scan_profile_dump();
        assert(WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0);
        assert(WaitForSingleObject(releaser, 5000) == WAIT_OBJECT_0);
        CloseHandle(worker);
        CloseHandle(releaser);
        CloseHandle(dispatch_entered);
        CloseHandle(release_dispatch);
        puts("dump drains an admitted writer and records its post-freeze completion");
        return 0;
    } else {
        return 2;
    }
    assert(!locked && qpc_calls && cycle_calls);
    spu_taskset_scan_profile_dump();
    puts("taskset scan profile fixture passed");
    return 0;
}
'''


def read_profile(path: Path):
    lines = path.read_text(encoding='ascii').splitlines()
    assert lines and lines[0].startswith('#qpc_frequency,')
    metadata_fields = lines[0][1:].split(',')
    metadata = dict(zip(metadata_fields[::2], metadata_fields[1::2]))
    rows = list(csv.DictReader(lines[1:]))
    return metadata, rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default=os.environ.get('CC', 'clang-cl'))
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='spu-taskset-scan-') as td:
        tmp = Path(td)
        out = args.output_dir.resolve() if args.output_dir else tmp
        out.mkdir(parents=True, exist_ok=True)
        source, exe, obj = tmp / 'fixture.c', tmp / 'fixture.exe', tmp / 'fixture.obj'
        source.write_text(HARNESS.replace('@@DISPATCH@@', SOURCE.as_posix()), encoding='utf-8')
        subprocess.run([args.compiler, '/nologo', '/TC', '/std:c17', '/W0', '/O1',
                        str(source), f'/Fe:{exe}', f'/Fo:{obj}'], check=True)

        disabled = {k: v for k, v in os.environ.items() if k != 'SPU_TASKSET_SCAN_PROFILE_CSV'}
        subprocess.run([str(exe), 'disabled'], check=True, env=disabled)

        profile, alternate = out / 'taskset-scan.csv', out / 'taskset-scan-alternate.csv'
        env = dict(os.environ, SPU_TASKSET_SCAN_PROFILE_CSV=str(profile))
        subprocess.run([str(exe), 'enabled', str(alternate)], check=True, env=env)
        metadata, rows = read_profile(profile)
        assert metadata['capacity'] == '4' and metadata['complete'] == '1'
        assert metadata['inflight'] == '0' and metadata['failure_reason'] == 'none'
        assert len(rows) == 2
        capture_start = int(metadata['capture_start_qpc'])
        capture_end = int(metadata['capture_end_qpc'])
        drain_end = int(metadata['drain_end_qpc'])
        assert capture_start <= min(int(row['begin_qpc']) for row in rows)
        assert max(int(row['end_qpc']) for row in rows) <= drain_end
        assert not alternate.exists(), 'selector changed after first scan'
        empty, claimed = rows
        assert int(empty['slots']) == 128 and int(empty['eligible']) == 0
        assert int(empty['claimed']) == 0 and int(empty['dispatch_attempts']) == 0
        assert int(claimed['slots']) == 128 and int(claimed['eligible']) == 1
        assert int(claimed['claimed']) == 1 and int(claimed['dispatch_attempts']) == 1
        assert int(claimed['dispatch_ok']) == 1 and int(claimed['dispatch_failed']) == 0
        assert int(claimed['prep_ticks']) >= 10000
        assert int(claimed['scan_ticks']) == int(claimed['total_ticks']) - int(claimed['prep_ticks'])
        assert int(claimed['prep_thread_cycles']) >= 20000
        assert int(claimed['scan_thread_cycles']) == (
            int(claimed['total_thread_cycles']) - int(claimed['prep_thread_cycles']))

        wrapped = out / 'taskset-scan-wrap.csv'
        subprocess.run([str(exe), 'wrap'], check=True,
                       env=dict(os.environ, SPU_TASKSET_SCAN_PROFILE_CSV=str(wrapped)))
        meta, rows = read_profile(wrapped)
        assert meta['capacity'] == '4' and meta['next_ticket'] == '6' and meta['overwritten'] == '2'
        assert [int(r['ticket']) for r in rows] == [2, 3, 4, 5]
        assert all(int(r['slots']) == 128 and int(r['claimed']) == 0 for r in rows)

        ringrace = out / 'taskset-scan-ringrace.csv'
        subprocess.run([str(exe), 'ringrace'], check=True,
                       env=dict(os.environ, SPU_TASKSET_SCAN_PROFILE_CSV=str(ringrace)))
        ring_meta, ring_rows = read_profile(ringrace)
        assert ring_meta['next_ticket'] == '5' and ring_meta['dropped_busy'] == '1'
        assert [int(r['ticket']) for r in ring_rows] == [0, 1, 2, 3]

        repeated = out / 'taskset-scan-repeat.csv'
        subprocess.run([str(exe), 'repeatdump'], check=True,
                       env=dict(os.environ, SPU_TASKSET_SCAN_PROFILE_CSV=str(repeated)))
        assert repeated.read_text(encoding='ascii').endswith('# fixture sentinel\n')

        timeout = out / 'taskset-scan-timeout.csv'
        subprocess.run([str(exe), 'timeout'], check=True,
                       env=dict(os.environ, SPU_TASKSET_SCAN_PROFILE_CSV=str(timeout)))
        timeout_meta, timeout_rows = read_profile(timeout)
        assert timeout_meta['complete'] == '0' and timeout_meta['inflight'] == '1'
        assert timeout_meta['failure_reason'] == 'writer_timeout' and not timeout_rows

        drain = out / 'taskset-scan-drain.csv'
        subprocess.run([str(exe), 'drain'], check=True,
                       env=dict(os.environ, SPU_TASKSET_SCAN_PROFILE_CSV=str(drain)))
        drain_meta, drain_rows = read_profile(drain)
        assert drain_meta['complete'] == '1' and drain_meta['inflight'] == '0'
        assert len(drain_rows) == 1
        assert int(drain_rows[0]['end_qpc']) > int(drain_meta['capture_end_qpc'])
        assert int(drain_rows[0]['end_qpc']) <= int(drain_meta['drain_end_qpc'])

        qpcfail = out / 'taskset-scan-qpcfail.csv'
        subprocess.run([str(exe), 'qpcfail'], check=True,
                       env=dict(os.environ, SPU_TASKSET_SCAN_PROFILE_CSV=str(qpcfail)))
        qpc_meta, qpc_rows = read_profile(qpcfail)
        assert qpc_meta['complete'] == '0' and qpc_meta['failure_reason'] == 'qpc_start'
        assert qpc_meta['discarded_qpc_invalid'] == '0' and not qpc_rows
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
