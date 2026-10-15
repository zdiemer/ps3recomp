/* Included once by spu_coherency.c so standalone coherency fixtures retain
 * their existing linkage and the disabled notify branch can inline. */
#include "spu_queue_diagnostic.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <stdatomic.h>
void spu_lockline_lock(void);
void spu_lockline_unlock(void);

static struct {
    uint32_t ea, buffer_ea, depth, tail, tail_known;
    unsigned char *header, *buffer;
    int active, stopped;
    uint64_t epoch, ordinal;
    struct { spu_queue_diag_row row; int valid; } pending[SPU_QUEUE_DIAG_DEPTH];
    struct { uint64_t generation, submit_qpc, ordinal; unsigned state; }
        slots[SPU_QUEUE_DIAG_SLOTS];
    spu_queue_diag_row rows[SPU_QUEUE_DIAG_CAPACITY];
    spu_queue_diag_snapshot totals;
} s_diag;
static atomic_uint s_installed;

static uint32_t diag_be32(const unsigned char* p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t diag_qpc(void)
{
    LARGE_INTEGER now = {0};
    return QueryPerformanceCounter(&now) && now.QuadPart > 0 ? (uint64_t)now.QuadPart : 0;
}
static uint64_t diag_pending(void)
{
    uint64_t count = 0;
    for (unsigned i = 0; i < s_diag.depth; ++i) count += s_diag.pending[i].valid != 0;
    return count;
}
static void diag_invalidate(void)
{
    s_diag.totals.pending_overwritten += diag_pending();
    memset(s_diag.pending, 0, sizeof(s_diag.pending));
    memset(s_diag.slots, 0, sizeof(s_diag.slots));
    s_diag.active = 0;
    atomic_store_explicit(&s_installed, 0, memory_order_release);
    s_diag.header = s_diag.buffer = NULL;
    ++s_diag.totals.invalidations;
}
static int diag_metadata_valid(void)
{
    const unsigned char* p = s_diag.header;
    return p && diag_be32(p + 8) == 16 && diag_be32(p + 12) == s_diag.depth &&
        diag_be32(p + 16) == 0 && diag_be32(p + 20) == s_diag.buffer_ea &&
        diag_be32(p + 28) == 1;
}
int spu_queue_diag_configure_locked(uint32_t ea, void* header, void* buffer,
    uint32_t buffer_ea, uint32_t depth)
{
    LARGE_INTEGER frequency = {0};
    if (s_diag.stopped || !ea || (ea & 127u) || !header || !buffer ||
            (buffer_ea & 15u) || !depth || depth > SPU_QUEUE_DIAG_DEPTH) return 0;
    if (s_diag.active && s_diag.ea == ea && s_diag.header == header &&
            s_diag.buffer == buffer && s_diag.buffer_ea == buffer_ea &&
            s_diag.depth == depth && diag_metadata_valid()) return 1;
    if (s_diag.active) diag_invalidate();
    s_diag.ea = ea; s_diag.header = header; s_diag.buffer = buffer;
    s_diag.buffer_ea = buffer_ea; s_diag.depth = depth;
    if (!diag_metadata_valid()) { s_diag.header = s_diag.buffer = NULL; return 0; }
    memset(s_diag.pending, 0, sizeof(s_diag.pending));
    memset(s_diag.slots, 0, sizeof(s_diag.slots));
    s_diag.tail = diag_be32(s_diag.header + 4);
    s_diag.tail_known = !(s_diag.tail & 0x80000000u) && s_diag.tail < depth * 2u;
    s_diag.active = 1;
    s_diag.epoch = ++s_diag.totals.epochs;
    if (!s_diag.totals.begin_qpc) s_diag.totals.begin_qpc = diag_qpc();
    if (!s_diag.totals.frequency && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
        s_diag.totals.frequency = (uint64_t)frequency.QuadPart;
    atomic_store_explicit(&s_installed, 1, memory_order_release);
    return 1;
}
void spu_queue_diag_submit_locked(uint32_t slot)
{
    if (!s_diag.active || slot >= SPU_QUEUE_DIAG_SLOTS) return;
    ++s_diag.totals.submits;
    ++s_diag.slots[slot].generation;
    s_diag.slots[slot].submit_qpc = diag_qpc();
    if (s_diag.slots[slot].state) {
        s_diag.slots[slot].state = 3; /* ambiguous until next owner epoch */
        ++s_diag.totals.ambiguous_submits;
    } else s_diag.slots[slot].state = 1;
}
void spu_queue_diag_notify_locked(uint32_t ea)
{
    uint32_t tail, index, slot;
    spu_queue_diag_row row = {0};
    if (!s_diag.active || (ea & ~127u) != s_diag.ea) return;
    if (!diag_metadata_valid()) { diag_invalidate(); return; }
    tail = diag_be32(s_diag.header + 4);
    if (tail & 0x80000000u) return; /* claim, not payload publication */
    if (tail >= s_diag.depth * 2u) { diag_invalidate(); return; }
    if (!s_diag.tail_known) { s_diag.tail = tail; s_diag.tail_known = 1; return; }
    if (tail == s_diag.tail) return; /* pop, failed write, or unrelated word */
    if (tail != (s_diag.tail + 1u) % (s_diag.depth * 2u)) {
        ++s_diag.totals.unsupported_tail;
        s_diag.totals.pending_overwritten += diag_pending();
        memset(s_diag.pending, 0, sizeof(s_diag.pending));
        for (unsigned i = 0; i < SPU_QUEUE_DIAG_SLOTS; ++i)
            if (s_diag.slots[i].state) s_diag.slots[i].state = 3;
        s_diag.tail = tail;
        return;
    }
    index = s_diag.tail;
    s_diag.tail = tail;
    slot = diag_be32(s_diag.buffer + (index % s_diag.depth) * 16u);
    row.epoch = s_diag.epoch; row.ordinal = ++s_diag.ordinal;
    row.queue = s_diag.ea; row.index = index; row.slot = slot;
    row.publication_qpc = diag_qpc(); row.producer_tid = GetCurrentThreadId();
    ++s_diag.totals.publications;
    if (slot < SPU_QUEUE_DIAG_SLOTS && s_diag.slots[slot].state == 1) {
        row.generation = s_diag.slots[slot].generation;
        row.submit_qpc = s_diag.slots[slot].submit_qpc;
        s_diag.slots[slot].ordinal = row.ordinal;
        s_diag.slots[slot].state = 2;
    } else {
        row.flags |= SPU_QUEUE_DIAG_NO_GENERATION;
        if (slot < SPU_QUEUE_DIAG_SLOTS && s_diag.slots[slot].state) {
            row.flags |= SPU_QUEUE_DIAG_AMBIGUOUS_GENERATION;
            s_diag.slots[slot].state = 3;
        }
    }
    if (s_diag.pending[index % s_diag.depth].valid) ++s_diag.totals.pending_overwritten;
    s_diag.pending[index % s_diag.depth].row = row;
    s_diag.pending[index % s_diag.depth].valid = 1;
}
void spu_queue_diag_pop_locked(uint32_t ea, uint32_t index, const void* record)
{
    spu_queue_diag_row row = {0};
    const uint32_t slot = record ? diag_be32(record) : UINT32_MAX;
    unsigned at;
    if (!s_diag.active || ea != s_diag.ea || index >= s_diag.depth * 2u) return;
    if (!diag_metadata_valid()) { diag_invalidate(); return; }
    at = index % s_diag.depth;
    if (s_diag.pending[at].valid && s_diag.pending[at].row.index == index &&
            s_diag.pending[at].row.epoch == s_diag.epoch) {
        row = s_diag.pending[at].row;
        s_diag.pending[at].valid = 0;
        if (row.slot != slot) row.flags |= SPU_QUEUE_DIAG_RECORD_MISMATCH;
        if (row.generation && (s_diag.slots[row.slot].state != 2 ||
                s_diag.slots[row.slot].generation != row.generation ||
                s_diag.slots[row.slot].ordinal != row.ordinal))
            row.flags |= SPU_QUEUE_DIAG_AMBIGUOUS_GENERATION;
        if (row.generation && !(row.flags & (SPU_QUEUE_DIAG_RECORD_MISMATCH |
                SPU_QUEUE_DIAG_AMBIGUOUS_GENERATION))) s_diag.slots[row.slot].state = 0;
    } else {
        row.epoch = s_diag.epoch; row.queue = ea; row.slot = slot; row.index = index;
        row.flags = SPU_QUEUE_DIAG_NO_PUBLICATION | SPU_QUEUE_DIAG_NO_GENERATION;
    }
    row.pop_qpc = diag_qpc(); row.consumer_tid = GetCurrentThreadId();
    if (!row.pop_qpc || (row.ordinal && !row.publication_qpc) ||
            (row.publication_qpc && row.pop_qpc < row.publication_qpc) ||
            (row.generation && (!row.submit_qpc || !row.publication_qpc ||
                row.publication_qpc < row.submit_qpc))) row.flags |= SPU_QUEUE_DIAG_BAD_CLOCK;
    row.sequence = s_diag.totals.recorded++;
    at = (unsigned)(row.sequence % SPU_QUEUE_DIAG_CAPACITY);
    if (row.sequence >= SPU_QUEUE_DIAG_CAPACITY &&
            s_diag.rows[at].pop_qpc > s_diag.totals.max_overwritten_pop_qpc)
        s_diag.totals.max_overwritten_pop_qpc = s_diag.rows[at].pop_qpc;
    s_diag.rows[at] = row;
}
void spu_queue_diag_reset(uint32_t ea)
{
    if (!atomic_load_explicit(&s_installed, memory_order_acquire)) return;
    spu_lockline_lock();
    if (s_diag.active && s_diag.ea == ea) diag_invalidate();
    spu_lockline_unlock();
}
void spu_queue_diag_stop(spu_queue_diag_snapshot* snapshot)
{
    spu_lockline_lock();
    if (!s_diag.stopped) {
        s_diag.totals.pending_at_stop = diag_pending();
        for (unsigned i = 0; i < s_diag.depth; ++i) {
            if (!s_diag.pending[i].valid) continue;
            const spu_queue_diag_row* row = &s_diag.pending[i].row;
            if (!row->publication_qpc) ++s_diag.totals.pending_unknown_publication_qpc;
            else if (!s_diag.totals.min_pending_publication_qpc ||
                    row->publication_qpc < s_diag.totals.min_pending_publication_qpc)
                s_diag.totals.min_pending_publication_qpc = row->publication_qpc;
            if (!row->submit_qpc) ++s_diag.totals.pending_unknown_submit_qpc;
            else if (!s_diag.totals.min_pending_submit_qpc ||
                    row->submit_qpc < s_diag.totals.min_pending_submit_qpc)
                s_diag.totals.min_pending_submit_qpc = row->submit_qpc;
        }
        s_diag.totals.end_qpc = diag_qpc();
    }
    s_diag.active = 0; s_diag.stopped = 1;
    s_diag.header = s_diag.buffer = NULL;
    atomic_store_explicit(&s_installed, 0, memory_order_release);
    s_diag.totals.rows = s_diag.rows;
    if (snapshot) *snapshot = s_diag.totals;
    spu_lockline_unlock();
}
#else
int spu_queue_diag_configure_locked(uint32_t ea, void* h, void* b, uint32_t bea, uint32_t depth)
{ (void)ea; (void)h; (void)b; (void)bea; (void)depth; return 0; }
void spu_queue_diag_submit_locked(uint32_t slot) { (void)slot; }
void spu_queue_diag_notify_locked(uint32_t ea) { (void)ea; }
void spu_queue_diag_pop_locked(uint32_t ea, uint32_t index, const void* record)
{ (void)ea; (void)index; (void)record; }
void spu_queue_diag_reset(uint32_t ea) { (void)ea; }
void spu_queue_diag_stop(spu_queue_diag_snapshot* snapshot)
{ if (snapshot) memset(snapshot, 0, sizeof(*snapshot)); }
#endif
