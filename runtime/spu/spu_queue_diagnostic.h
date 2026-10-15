#ifndef SPU_QUEUE_DIAGNOSTIC_H
#define SPU_QUEUE_DIAGNOSTIC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef SPU_QUEUE_DIAG_CAPACITY
#define SPU_QUEUE_DIAG_CAPACITY 262144u
#endif
#define SPU_QUEUE_DIAG_DEPTH 1024u
#define SPU_QUEUE_DIAG_SLOTS 512u
enum {
    SPU_QUEUE_DIAG_NO_PUBLICATION = 1u,
    SPU_QUEUE_DIAG_NO_GENERATION = 2u,
    SPU_QUEUE_DIAG_AMBIGUOUS_GENERATION = 4u,
    SPU_QUEUE_DIAG_RECORD_MISMATCH = 8u,
    SPU_QUEUE_DIAG_BAD_CLOCK = 16u
};
typedef struct spu_queue_diag_row {
    uint64_t sequence, epoch, ordinal, generation;
    uint64_t submit_qpc, publication_qpc, pop_qpc;
    uint32_t queue, slot, index, flags, producer_tid, consumer_tid;
} spu_queue_diag_row;
typedef struct spu_queue_diag_snapshot {
    const spu_queue_diag_row* rows;
    uint64_t recorded, frequency, epochs, publications, submits;
    uint64_t invalidations, unsupported_tail, pending_overwritten, pending_at_stop;
    uint64_t ambiguous_submits, max_overwritten_pop_qpc, begin_qpc, end_qpc;
    /* Published records not popped at stop. Minima ignore zero (unknown)
     * clocks, which are counted separately. Submit minimum covers these
     * pending records only, not jobs that have yet to publish anything. */
    uint64_t min_pending_publication_qpc, min_pending_submit_qpc;
    uint64_t pending_unknown_publication_qpc, pending_unknown_submit_qpc;
} spu_queue_diag_snapshot;
/* One validated queue. All *_locked calls require the existing coherency
 * lock. Header and buffer mappings must stay valid until reset/stop. No guest
 * writes, allocation, file I/O, native waits or client callbacks occur here.
 * Generations count actual guarded submit events, independently of job tracing.
 * Queue epoch changes on reconfiguration; uncertain records stay unmatched. */
int spu_queue_diag_configure_locked(uint32_t ea, void* header, void* buffer,
    uint32_t buffer_ea, uint32_t depth);
void spu_queue_diag_submit_locked(uint32_t slot);
void spu_queue_diag_notify_locked(uint32_t ea);
void spu_queue_diag_pop_locked(uint32_t ea, uint32_t index, const void* record);
/* Reset is called BEFORE queue initialization overwrites the old header. It
 * takes the coherency lock only when this diagnostic has been installed. */
void spu_queue_diag_reset(uint32_t ea);
/* Permanently stops this process's diagnostic and returns immutable RAM rows.
 * Caller performs all output after this function releases the coherency lock. */
void spu_queue_diag_stop(spu_queue_diag_snapshot* snapshot);
#ifdef __cplusplus
}
#endif
#endif
