/* Included once after g_lockline in spu_coherency.c. */
#include <stdatomic.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define COH_PROFILE_CALLER() ((uintptr_t)_ReturnAddress())
#elif defined(__GNUC__)
#define COH_PROFILE_CALLER() ((uintptr_t)__builtin_return_address(0))
#else
#define COH_PROFILE_CALLER() ((uintptr_t)0)
#endif
#define COH_PROFILE_CLOSED 0x80000000u
#define COH_PROFILE_OVERFLOW 0x40000000u
#define COH_PROFILE_COUNT 0x3fffffffu
static atomic_uint s_coh_profile_enabled;
static atomic_uint s_coh_profile_gate = ATOMIC_VAR_INIT(COH_PROFILE_CLOSED);
static unsigned s_coh_profile_state; /* 0 unstarted, 1 active, 2 frozen; lock-owned */
static spu_coh_lock_profile_snapshot s_coh_profile_totals;
static spu_coh_lock_profile_row s_coh_profile_rows[SPU_COH_LOCK_PROFILE_CAPACITY];

static uint64_t coh_profile_qpc(void)
{
    LARGE_INTEGER value = {0};
    return QueryPerformanceCounter(&value) && value.QuadPart > 0 ? (uint64_t)value.QuadPart : 0;
}
static int coh_profile_admit(void)
{
    unsigned gate = atomic_load_explicit(&s_coh_profile_gate, memory_order_acquire);
    for (;;) {
        if (gate & COH_PROFILE_CLOSED) return 0;
        unsigned next = (gate & COH_PROFILE_COUNT) == COH_PROFILE_COUNT ?
            gate | COH_PROFILE_OVERFLOW : gate + 1u;
        if (atomic_compare_exchange_weak_explicit(&s_coh_profile_gate, &gate, next,
                memory_order_acq_rel, memory_order_acquire))
            return (gate & COH_PROFILE_COUNT) != COH_PROFILE_COUNT;
    }
}
static void coh_profile_record_locked(uint64_t begin, uint64_t end,
    uint64_t caller, uint32_t tid)
{
    if (s_coh_profile_state == 1) {
        ++s_coh_profile_totals.completed_contentions;
        if (!begin || !end || end < begin) ++s_coh_profile_totals.invalid_clocks;
        else if (end - begin < s_coh_profile_totals.threshold_ticks)
            ++s_coh_profile_totals.below_threshold;
        else {
            const uint64_t sequence = s_coh_profile_totals.recorded++;
            spu_coh_lock_profile_row* row = &s_coh_profile_rows[sequence % SPU_COH_LOCK_PROFILE_CAPACITY];
            if (sequence >= SPU_COH_LOCK_PROFILE_CAPACITY &&
                    row->end_qpc > s_coh_profile_totals.max_overwritten_end_qpc)
                s_coh_profile_totals.max_overwritten_end_qpc = row->end_qpc;
            row->sequence = sequence; row->begin_qpc = begin; row->end_qpc = end;
            row->caller = caller; row->host_tid = tid; row->reserved = 0;
        }
    }
    atomic_fetch_sub_explicit(&s_coh_profile_gate, 1u, memory_order_release);
}
int spu_coh_lock_profile_start(void)
{
    int result = 0;
    AcquireSRWLockExclusive(&g_lockline);
    if (s_coh_profile_state == 1) result = 1;
    else if (!s_coh_profile_state) {
        LARGE_INTEGER frequency = {0};
        const uint64_t begin = coh_profile_qpc();
        if (begin && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
            s_coh_profile_totals.frequency = (uint64_t)frequency.QuadPart;
            s_coh_profile_totals.threshold_ticks = ((uint64_t)frequency.QuadPart + 999u) / 1000u;
            s_coh_profile_totals.begin_qpc = begin;
            s_coh_profile_totals.image_base = (uint64_t)(uintptr_t)GetModuleHandleW(NULL);
            s_coh_profile_totals.started = 1;
            s_coh_profile_state = 1;
            atomic_store_explicit(&s_coh_profile_gate, 0, memory_order_release);
            atomic_store_explicit(&s_coh_profile_enabled, 1, memory_order_release);
            result = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_lockline);
    return result;
}
void spu_coh_lock_profile_stop(spu_coh_lock_profile_snapshot* snapshot)
{
    AcquireSRWLockExclusive(&g_lockline);
    if (s_coh_profile_state != 2) {
        atomic_store_explicit(&s_coh_profile_enabled, 0, memory_order_release);
        const unsigned gate = atomic_fetch_or_explicit(&s_coh_profile_gate,
            COH_PROFILE_CLOSED, memory_order_acq_rel);
        s_coh_profile_totals.inflight_at_stop = gate & COH_PROFILE_COUNT;
        s_coh_profile_totals.admission_overflow = (gate & COH_PROFILE_OVERFLOW) != 0;
        s_coh_profile_totals.admitted_contentions =
            s_coh_profile_totals.completed_contentions + s_coh_profile_totals.inflight_at_stop;
        if (s_coh_profile_state == 1) s_coh_profile_totals.end_qpc = coh_profile_qpc();
        s_coh_profile_totals.clock_valid = s_coh_profile_totals.started &&
            s_coh_profile_totals.end_qpc >= s_coh_profile_totals.begin_qpc &&
            s_coh_profile_totals.begin_qpc != 0;
        s_coh_profile_state = 2;
    }
    s_coh_profile_totals.rows = s_coh_profile_rows;
    if (snapshot) *snapshot = s_coh_profile_totals;
    ReleaseSRWLockExclusive(&g_lockline);
}
int spu_coh_lock_profile_dump(const char* path)
{
    spu_coh_lock_profile_snapshot snapshot;
    spu_coh_lock_profile_stop(&snapshot);
    if (!path || !*path || strlen(path) >= 4096) return 0;
    FILE* file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fprintf(file, "qpc_frequency,sequence,begin_qpc,end_qpc,host_tid,caller\n") >= 0;
    const uint64_t first = snapshot.recorded > SPU_COH_LOCK_PROFILE_CAPACITY ?
        snapshot.recorded - SPU_COH_LOCK_PROFILE_CAPACITY : 0;
    for (uint64_t i = first; i < snapshot.recorded; ++i) {
        const spu_coh_lock_profile_row* row = &snapshot.rows[i % SPU_COH_LOCK_PROFILE_CAPACITY];
        if (fprintf(file, "%llu,%llu,%llu,%llu,%u,%llu\n", snapshot.frequency,
                row->sequence, row->begin_qpc, row->end_qpc, row->host_tid, row->caller) < 0) ok = 0;
    }
    if (fclose(file)) ok = 0;
    if (!ok) return 0;
    char metadata_path[4112];
    snprintf(metadata_path, sizeof metadata_path, "%s.meta.json", path);
    file = fopen(metadata_path, "wb");
    if (!file) return 0;
    ok = fprintf(file, "{\n  \"schema\": 1,\n  \"started\": %u,\n  \"capture_clock_valid\": %u,\n"
        "  \"qpc_frequency\": %llu,\n  \"begin_qpc\": %llu,\n  \"end_qpc\": %llu,\n"
        "  \"image_base\": %llu,\n  \"capacity\": %u,\n  \"threshold_us\": %u,\n  \"threshold_ticks\": %llu,\n"
        "  \"admitted_contentions\": %llu,\n  \"completed_contentions\": %llu,\n"
        "  \"below_threshold\": %llu,\n  \"invalid_clock_discarded\": %llu,\n"
        "  \"recorded\": %llu,\n  \"retained\": %llu,\n  \"overwritten\": %llu,\n"
        "  \"max_overwritten_end_qpc\": %llu,\n  \"inflight_at_stop\": %llu,\n"
        "  \"admission_overflow\": %u,\n"
        "  \"interpretation\": \"Contended acquisition wall intervals after failed TryAcquire; "
        "includes owner hold and scheduling. No uncontended QPC or short-wait distribution. "
        "Invalid clocks and admitted unfinished waits are discarded and prevent full coverage. "
        "Caller is a raw return-address hint; image_base supports RVA resolution.\"\n}\n",
        snapshot.started, snapshot.clock_valid, snapshot.frequency, snapshot.begin_qpc,
        snapshot.end_qpc, snapshot.image_base, (unsigned)SPU_COH_LOCK_PROFILE_CAPACITY,
        (unsigned)SPU_COH_LOCK_PROFILE_THRESHOLD_US, snapshot.threshold_ticks,
        snapshot.admitted_contentions, snapshot.completed_contentions,
        snapshot.below_threshold, snapshot.invalid_clocks, snapshot.recorded,
        snapshot.recorded-first, first, snapshot.max_overwritten_end_qpc,
        snapshot.inflight_at_stop, snapshot.admission_overflow) >= 0;
    if (fclose(file)) ok = 0;
    return ok;
}
