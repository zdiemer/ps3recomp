#include "rsx_native_draw_payload.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void* default_reallocate(void* user, void* old_memory, size_t new_size)
{
    (void)user;
    return realloc(old_memory, new_size);
}

static void default_release(void* user, void* memory)
{
    (void)user;
    free(memory);
}

static int payload_fail(rsx_native_draw_payload_store* store)
{
    if (store && store->state == RSX_NATIVE_DRAW_PAYLOAD_OPEN)
        store->state = RSX_NATIVE_DRAW_PAYLOAD_FAILED;
    return 0;
}

static int align_size(size_t value, size_t alignment, size_t* out)
{
    size_t remainder;
    size_t padding;
    if (!out || alignment == 0)
        return 0;
    remainder = value % alignment;
    padding = remainder ? alignment - remainder : 0;
    if (value > SIZE_MAX - padding)
        return 0;
    *out = value + padding;
    return 1;
}

static int reserve_bytes(rsx_native_draw_payload_store* store, size_t need)
{
    size_t capacity;
    uint8_t* replacement;
    if (need > store->bytes_limit)
        return 0;
    if (need <= store->bytes_capacity)
        return 1;
    capacity = store->bytes_capacity ? store->bytes_capacity : 4096u;
    if (capacity > store->bytes_limit)
        capacity = store->bytes_limit;
    while (capacity < need) {
        size_t next;
        if (capacity > SIZE_MAX / 2u)
            next = store->bytes_limit;
        else
            next = capacity * 2u;
        if (next > store->bytes_limit)
            next = store->bytes_limit;
        if (next <= capacity)
            return 0;
        capacity = next;
    }
    replacement = (uint8_t*)store->allocator.reallocate(
        store->allocator.user, store->bytes, capacity);
    if (!replacement)
        return 0;
    store->bytes = replacement;
    store->bytes_capacity = capacity;
    return 1;
}

static int reserve_records(rsx_native_draw_payload_store* store, uint32_t need)
{
    uint32_t capacity;
    rsx_native_draw_payload_record* replacement;
    size_t allocation_size;
    if (need > store->records_limit)
        return 0;
    if (need <= store->records_capacity)
        return 1;
    capacity = store->records_capacity ? store->records_capacity : 16u;
    if (capacity > store->records_limit)
        capacity = store->records_limit;
    while (capacity < need) {
        uint32_t next = capacity > UINT32_MAX / 2u
            ? store->records_limit : capacity * 2u;
        if (next > store->records_limit)
            next = store->records_limit;
        if (next <= capacity)
            return 0;
        capacity = next;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof(*store->records))
        return 0;
    allocation_size = (size_t)capacity * sizeof(*store->records);
    replacement = (rsx_native_draw_payload_record*)store->allocator.reallocate(
        store->allocator.user, store->records, allocation_size);
    if (!replacement)
        return 0;
    store->records = replacement;
    store->records_capacity = capacity;
    return 1;
}

static int validate_span(const rsx_native_draw_payload_span* span,
                         int required)
{
    if (!span)
        return 0;
    if (span->size == 0)
        return !required && span->data == NULL;
    return span->data != NULL;
}

static int spans_overlap_storage(
    const rsx_native_draw_payload_store* store,
    const rsx_native_draw_payload_span* span)
{
    uintptr_t begin, end;
    uintptr_t storage_begin, storage_end;
    if (!span->size)
        return 0;
    begin = (uintptr_t)span->data;
    if (span->size > UINTPTR_MAX - begin)
        return 1;
    end = begin + span->size;
    if (store->bytes) {
        storage_begin = (uintptr_t)store->bytes;
        if (store->bytes_capacity > UINTPTR_MAX - storage_begin)
            return 1;
        storage_end = storage_begin + store->bytes_capacity;
        if (begin < storage_end && storage_begin < end)
            return 1;
    }
    if (store->records) {
        storage_begin = (uintptr_t)store->records;
        if ((size_t)store->records_capacity >
            SIZE_MAX / sizeof(*store->records))
            return 1;
        {
            const size_t storage_size =
                (size_t)store->records_capacity * sizeof(*store->records);
            if (storage_size > UINTPTR_MAX - storage_begin)
                return 1;
            storage_end = storage_begin + storage_size;
        }
        if (begin < storage_end && storage_begin < end)
            return 1;
    }
    return 0;
}

static int append_span(size_t* cursor,
                       const rsx_native_draw_payload_span* span,
                       uint64_t* out_offset,
                       uint64_t* out_size)
{
    size_t offset;
    if (!cursor || !span || !out_offset || !out_size)
        return 0;
    if (!span->size) {
        *out_offset = 0;
        *out_size = 0;
        return 1;
    }
    if (!align_size(*cursor, RSX_NATIVE_DRAW_PAYLOAD_ALIGNMENT, &offset) ||
        offset > SIZE_MAX - span->size)
        return 0;
    *out_offset = (uint64_t)offset;
    *out_size = (uint64_t)span->size;
    *cursor = offset + span->size;
    return 1;
}

int rsx_native_draw_payload_init(
    rsx_native_draw_payload_store* store,
    size_t max_bytes,
    uint32_t max_draws)
{
    return rsx_native_draw_payload_init_with_allocator(
        store, max_bytes, max_draws, NULL);
}

int rsx_native_draw_payload_init_with_allocator(
    rsx_native_draw_payload_store* store,
    size_t max_bytes,
    uint32_t max_draws,
    const rsx_native_draw_payload_allocator* allocator)
{
    if (!store || !max_bytes || !max_draws)
        return 0;
    if (allocator && (!allocator->reallocate || !allocator->release))
        return 0;
    memset(store, 0, sizeof(*store));
    store->bytes_limit = max_bytes;
    store->records_limit = max_draws;
    store->allocator.reallocate = allocator
        ? allocator->reallocate : default_reallocate;
    store->allocator.release = allocator
        ? allocator->release : default_release;
    store->allocator.user = allocator ? allocator->user : NULL;
    store->state = RSX_NATIVE_DRAW_PAYLOAD_OPEN;
    return 1;
}

int rsx_native_draw_payload_append(
    rsx_native_draw_payload_store* store,
    const rsx_native_draw_payload_input* input,
    uint32_t* out_index)
{
    rsx_native_draw_payload_record record;
    rsx_native_draw_payload_input input_copy;
    const rsx_native_draw_payload_input* stable_input;
    size_t end;
    size_t cursor;
    uint32_t next_count;
    if (!store || store->state != RSX_NATIVE_DRAW_PAYLOAD_OPEN)
        return 0;
    if (!input)
        return payload_fail(store);
    /* The caller may keep the descriptor beside this store. Copy it before
     * either reserve can realloc the record or byte arena. Span data itself
     * may not alias store storage (checked below). */
    input_copy = *input;
    stable_input = &input_copy;
    if (!validate_span(&stable_input->vertex, 0) ||
        !validate_span(&stable_input->index, 0) ||
        !validate_span(&stable_input->vertex_constants, 1) ||
        !validate_span(&stable_input->pixel_constants, 0) ||
        spans_overlap_storage(store, &stable_input->vertex) ||
        spans_overlap_storage(store, &stable_input->index) ||
        spans_overlap_storage(store, &stable_input->vertex_constants) ||
        spans_overlap_storage(store, &stable_input->pixel_constants))
        return payload_fail(store);
    if (store->record_count >= store->records_limit ||
        store->record_count == UINT32_MAX)
        return payload_fail(store);
    next_count = store->record_count + 1u;

    memset(&record, 0, sizeof(record));
    record.draw_ordinal = stable_input->draw_ordinal;
    end = store->bytes_used;
    if (!append_span(&end, &stable_input->vertex,
                     &record.vertex_offset, &record.vertex_size) ||
        !append_span(&end, &stable_input->index,
                     &record.index_offset, &record.index_size) ||
        !append_span(&end, &stable_input->vertex_constants,
                     &record.vertex_constants_offset,
                     &record.vertex_constants_size) ||
        !append_span(&end, &stable_input->pixel_constants,
                     &record.pixel_constants_offset,
                     &record.pixel_constants_size) ||
        end > store->bytes_limit)
        return payload_fail(store);

    /* Reserve both backing stores before copying or publishing anything. */
    if (!reserve_records(store, next_count) || !reserve_bytes(store, end))
        return payload_fail(store);

    cursor = store->bytes_used;
#define COPY_PAYLOAD_SPAN(offset_field, size_field, input_field) do { \
        if (record.size_field) { \
            const size_t span_offset = (size_t)record.offset_field; \
            if (span_offset > cursor) \
                memset(store->bytes + cursor, 0, span_offset - cursor); \
            memcpy(store->bytes + span_offset, stable_input->input_field.data, \
                   (size_t)record.size_field); \
            cursor = span_offset + (size_t)record.size_field; \
        } \
    } while (0)
    COPY_PAYLOAD_SPAN(vertex_offset, vertex_size, vertex);
    COPY_PAYLOAD_SPAN(index_offset, index_size, index);
    COPY_PAYLOAD_SPAN(vertex_constants_offset, vertex_constants_size,
                      vertex_constants);
    COPY_PAYLOAD_SPAN(pixel_constants_offset, pixel_constants_size,
                      pixel_constants);
#undef COPY_PAYLOAD_SPAN

    store->records[store->record_count] = record;
    if (out_index)
        *out_index = store->record_count;
    store->record_count = next_count;
    store->bytes_used = end;
    return 1;
}

int rsx_native_draw_payload_seal(rsx_native_draw_payload_store* store)
{
    if (!store || store->state != RSX_NATIVE_DRAW_PAYLOAD_OPEN)
        return 0;
    store->state = RSX_NATIVE_DRAW_PAYLOAD_SEALED;
    return 1;
}

int rsx_native_draw_payload_reset(rsx_native_draw_payload_store* store)
{
    if (!store || store->state == RSX_NATIVE_DRAW_PAYLOAD_UNINITIALIZED)
        return 0;
    store->bytes_used = 0;
    store->record_count = 0;
    store->state = RSX_NATIVE_DRAW_PAYLOAD_OPEN;
    return 1;
}

void rsx_native_draw_payload_destroy(rsx_native_draw_payload_store* store)
{
    if (!store)
        return;
    if (store->allocator.release) {
        if (store->bytes)
            store->allocator.release(store->allocator.user, store->bytes);
        if (store->records)
            store->allocator.release(store->allocator.user, store->records);
    } else {
        free(store->bytes);
        free(store->records);
    }
    memset(store, 0, sizeof(*store));
}

uint32_t rsx_native_draw_payload_count(
    const rsx_native_draw_payload_store* store)
{
    return store && store->state != RSX_NATIVE_DRAW_PAYLOAD_UNINITIALIZED
        ? store->record_count : 0;
}

rsx_native_draw_payload_state rsx_native_draw_payload_get_state(
    const rsx_native_draw_payload_store* store)
{
    return store ? store->state : RSX_NATIVE_DRAW_PAYLOAD_UNINITIALIZED;
}

int rsx_native_draw_payload_get_record(
    const rsx_native_draw_payload_store* store,
    uint32_t index,
    rsx_native_draw_payload_record* out_record)
{
    if (!store || !out_record ||
        (store->state != RSX_NATIVE_DRAW_PAYLOAD_OPEN &&
         store->state != RSX_NATIVE_DRAW_PAYLOAD_SEALED) ||
        index >= store->record_count)
        return 0;
    *out_record = store->records[index];
    return 1;
}

int rsx_native_draw_payload_get_bytes(
    const rsx_native_draw_payload_store* store,
    const rsx_native_draw_payload_record* record,
    rsx_native_draw_payload_kind kind,
    const void** out_data,
    size_t* out_size)
{
    uint64_t offset;
    uint64_t size;
    if (out_data)
        *out_data = NULL;
    if (out_size)
        *out_size = 0;
    if (!store || !record || !out_data || !out_size ||
        (store->state != RSX_NATIVE_DRAW_PAYLOAD_OPEN &&
         store->state != RSX_NATIVE_DRAW_PAYLOAD_SEALED))
        return 0;
    switch (kind) {
    case RSX_NATIVE_DRAW_PAYLOAD_VERTEX:
        offset = record->vertex_offset; size = record->vertex_size; break;
    case RSX_NATIVE_DRAW_PAYLOAD_INDEX:
        offset = record->index_offset; size = record->index_size; break;
    case RSX_NATIVE_DRAW_PAYLOAD_VERTEX_CONSTANTS:
        offset = record->vertex_constants_offset;
        size = record->vertex_constants_size; break;
    case RSX_NATIVE_DRAW_PAYLOAD_PIXEL_CONSTANTS:
        offset = record->pixel_constants_offset;
        size = record->pixel_constants_size; break;
    default:
        return 0;
    }
    if (!size || offset > SIZE_MAX || size > SIZE_MAX ||
        (size_t)offset > store->bytes_used ||
        (size_t)size > store->bytes_used - (size_t)offset)
        return 0;
    *out_data = store->bytes + (size_t)offset;
    *out_size = (size_t)size;
    return 1;
}
