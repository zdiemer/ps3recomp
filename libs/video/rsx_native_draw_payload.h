#ifndef PS3RECOMP_RSX_NATIVE_DRAW_PAYLOAD_H
#define PS3RECOMP_RSX_NATIVE_DRAW_PAYLOAD_H

/*
 * Bounded, source-independent ownership for CPU inputs to native draws.
 * This stores bytes and draw ordinals only. It does not own GPU resources,
 * descriptors, render targets, or replay state.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_NATIVE_DRAW_PAYLOAD_ALIGNMENT 256u

typedef enum rsx_native_draw_payload_state {
    RSX_NATIVE_DRAW_PAYLOAD_UNINITIALIZED = 0,
    RSX_NATIVE_DRAW_PAYLOAD_OPEN = 1,
    RSX_NATIVE_DRAW_PAYLOAD_SEALED = 2,
    RSX_NATIVE_DRAW_PAYLOAD_FAILED = 3
} rsx_native_draw_payload_state;

typedef struct rsx_native_draw_payload_span {
    const void* data;
    size_t size;
} rsx_native_draw_payload_span;

typedef struct rsx_native_draw_payload_input {
    uint64_t draw_ordinal;
    rsx_native_draw_payload_span vertex;
    rsx_native_draw_payload_span index;             /* optional */
    rsx_native_draw_payload_span vertex_constants;
    rsx_native_draw_payload_span pixel_constants;   /* optional */
} rsx_native_draw_payload_input;

typedef struct rsx_native_draw_payload_record {
    uint64_t draw_ordinal;
    uint64_t vertex_offset;
    uint64_t vertex_size;
    uint64_t index_offset;
    uint64_t index_size;
    uint64_t vertex_constants_offset;
    uint64_t vertex_constants_size;
    uint64_t pixel_constants_offset;
    uint64_t pixel_constants_size;
} rsx_native_draw_payload_record;

typedef void* (*rsx_native_draw_payload_realloc_fn)(
    void* user, void* old_memory, size_t new_size);
typedef void (*rsx_native_draw_payload_free_fn)(void* user, void* memory);
typedef struct rsx_native_draw_payload_allocator {
    rsx_native_draw_payload_realloc_fn reallocate;
    rsx_native_draw_payload_free_fn release;
    void* user;
} rsx_native_draw_payload_allocator;

typedef struct rsx_native_draw_payload_store {
    uint8_t* bytes;
    rsx_native_draw_payload_record* records;
    size_t bytes_used;
    size_t bytes_capacity;
    size_t bytes_limit;
    uint32_t record_count;
    uint32_t records_capacity;
    uint32_t records_limit;
    rsx_native_draw_payload_state state;
    rsx_native_draw_payload_allocator allocator;
} rsx_native_draw_payload_store;

typedef enum rsx_native_draw_payload_kind {
    RSX_NATIVE_DRAW_PAYLOAD_VERTEX = 0,
    RSX_NATIVE_DRAW_PAYLOAD_INDEX = 1,
    RSX_NATIVE_DRAW_PAYLOAD_VERTEX_CONSTANTS = 2,
    RSX_NATIVE_DRAW_PAYLOAD_PIXEL_CONSTANTS = 3
} rsx_native_draw_payload_kind;

/* Initialization is allocation-free. Limits must be nonzero. The destination
 * must be fresh storage or previously destroyed; it must not contain a live
 * initialized store, because initialization zeroes the struct. */
int rsx_native_draw_payload_init(
    rsx_native_draw_payload_store* store,
    size_t max_bytes,
    uint32_t max_draws);

/* Optional allocator injection supports deterministic allocation-failure
 * tests. Both callbacks are required; NULL selects the standard allocator. */
int rsx_native_draw_payload_init_with_allocator(
    rsx_native_draw_payload_store* store,
    size_t max_bytes,
    uint32_t max_draws,
    const rsx_native_draw_payload_allocator* allocator);

/* While OPEN, any invalid input, capacity failure, or allocation failure makes
 * the store FAILED and publishes no record. Other states reject append without
 * changing state. out_index is untouched on fail. Alignment applies to offsets
 * within the arena; malloc's base address is not promised to be 256-aligned.
 */
int rsx_native_draw_payload_append(
    rsx_native_draw_payload_store* store,
    const rsx_native_draw_payload_input* input,
    uint32_t* out_index);

/* Only a complete OPEN store can be sealed. FAILED stores cannot be sealed. */
int rsx_native_draw_payload_seal(rsx_native_draw_payload_store* store);

/* Reset explicitly starts a new capture and retains already allocated memory. */
int rsx_native_draw_payload_reset(rsx_native_draw_payload_store* store);
void rsx_native_draw_payload_destroy(rsx_native_draw_payload_store* store);

uint32_t rsx_native_draw_payload_count(
    const rsx_native_draw_payload_store* store);
rsx_native_draw_payload_state rsx_native_draw_payload_get_state(
    const rsx_native_draw_payload_store* store);

int rsx_native_draw_payload_get_record(
    const rsx_native_draw_payload_store* store,
    uint32_t index,
    rsx_native_draw_payload_record* out_record);

/* Returns a pointer only when the requested span is present and entirely
 * within the currently published byte range. The pointer remains valid until
 * the next append/reset/destroy on this store. */
int rsx_native_draw_payload_get_bytes(
    const rsx_native_draw_payload_store* store,
    const rsx_native_draw_payload_record* record,
    rsx_native_draw_payload_kind kind,
    const void** out_data,
    size_t* out_size);

#ifdef __cplusplus
}
#endif

#endif
