/* Link the actual rsx_native_draw_payload.c; no copied implementation.
 * Standalone driver in a VS developer environment:
 * python -m unittest discover -s libs/video/tests -p test_native_draw_payload.py -v
 * clang-cl /nologo /TC /W4 /WX /O2 /I<video> test_native_draw_payload.c
 *   <video>/rsx_native_draw_payload.c /Fo<temp>/ /Fe<temp>/test.exe
 */
#ifdef NDEBUG
#error This fixture requires active assertions
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_native_draw_payload.h"

typedef struct allocation { void* memory; size_t size; } allocation;
typedef struct allocator_state {
    allocation live[4];
    unsigned calls, fail_call, releases, live_count;
    size_t largest_request;
} allocator_state;
static unsigned char data[4][6000];

static void* moving_reallocate(void* user, void* old, size_t size) {
    allocator_state* a = (allocator_state*)user;
    unsigned slot = 4;
    assert(size && size <= 1024u*1024u); /* Reject unintended giant test allocations. */
    ++a->calls;
    if (size > a->largest_request) a->largest_request = size;
    if (a->calls == a->fail_call) return NULL;
    for (unsigned i = 0; i < 4; ++i)
        if (old ? a->live[i].memory == old : a->live[i].memory == NULL) { slot = i; break; }
    assert(slot < 4);
    void* replacement = malloc(size);
    assert(replacement);
    memset(replacement, 0xA5, size);
    if (old) {
        memcpy(replacement, old, size < a->live[slot].size ? size : a->live[slot].size);
        free(old); /* Always relocate; catches use of a borrowed old allocation. */
    } else ++a->live_count;
    a->live[slot].memory = replacement; a->live[slot].size = size;
    return replacement;
}
static void moving_release(void* user, void* memory) {
    allocator_state* a = (allocator_state*)user;
    if (!memory) return;
    for (unsigned i = 0; i < 4; ++i) if (a->live[i].memory == memory) {
        free(memory); a->live[i].memory = NULL; a->live[i].size = 0;
        ++a->releases; --a->live_count; return;
    }
    assert(!"unknown or double free");
}
static void init(rsx_native_draw_payload_store* s, allocator_state* a, size_t bytes, uint32_t draws) {
    rsx_native_draw_payload_allocator allocator = { moving_reallocate, moving_release, a };
    memset(s, 0, sizeof(*s)); memset(a, 0, sizeof(*a));
    assert(rsx_native_draw_payload_init_with_allocator(s, bytes, draws, &allocator));
    assert(a->calls == 0 && !s->bytes && !s->records);
    assert(rsx_native_draw_payload_get_state(s) == RSX_NATIVE_DRAW_PAYLOAD_OPEN);
}
static void destroy(rsx_native_draw_payload_store* s, allocator_state* a) {
    rsx_native_draw_payload_store zero;
    memset(&zero, 0, sizeof(zero));
    rsx_native_draw_payload_destroy(s);
    assert(a->live_count == 0 && !memcmp(s, &zero, sizeof(zero)));
    rsx_native_draw_payload_destroy(s);
    assert(a->live_count == 0 && !memcmp(s, &zero, sizeof(zero)));
}
static void fill_data(unsigned seed) {
    for (unsigned k = 0; k < 4; ++k)
        for (unsigned i = 0; i < sizeof(data[k]); ++i) data[k][i] = (unsigned char)(seed+17u*k+3u*i);
}
static rsx_native_draw_payload_input input(uint64_t ordinal) {
    rsx_native_draw_payload_input in;
    memset(&in, 0, sizeof(in)); in.draw_ordinal = ordinal;
    in.vertex.data = data[0]; in.vertex.size = 3;
    in.index.data = data[1]; in.index.size = 5;
    in.vertex_constants.data = data[2]; in.vertex_constants.size = 7;
    in.pixel_constants.data = data[3]; in.pixel_constants.size = 11;
    return in;
}
static rsx_native_draw_payload_input constants_only(uint64_t ordinal) {
    rsx_native_draw_payload_input in = input(ordinal);
    in.vertex.data = NULL; in.vertex.size = 0;
    in.index.data = NULL; in.index.size = 0;
    in.pixel_constants.data = NULL; in.pixel_constants.size = 0;
    in.vertex_constants.size = 1;
    return in;
}
static void check_span(const rsx_native_draw_payload_store* s, const rsx_native_draw_payload_record* r,
                       rsx_native_draw_payload_kind kind, const void* wanted, size_t size) {
    const void* bytes = (const void*)(uintptr_t)1; size_t actual = SIZE_MAX;
    int ok = rsx_native_draw_payload_get_bytes(s, r, kind, &bytes, &actual);
    assert(ok == (size != 0));
    if (size) assert(bytes && actual == size && !memcmp(bytes, wanted, size));
    else assert(bytes == NULL && actual == 0);
}
static void assert_failed(rsx_native_draw_payload_store* s, uint32_t count, size_t used) {
    rsx_native_draw_payload_record r;
    const void* bytes = (const void*)(uintptr_t)1; size_t size = SIZE_MAX;
    assert(s->state == RSX_NATIVE_DRAW_PAYLOAD_FAILED && s->record_count == count && s->bytes_used == used);
    assert(rsx_native_draw_payload_count(s) == count);
    memset(&r, 0, sizeof(r)); r.vertex_constants_size = 1;
    assert(!rsx_native_draw_payload_get_record(s, 0, &r));
    assert(!rsx_native_draw_payload_get_bytes(s, &r, RSX_NATIVE_DRAW_PAYLOAD_VERTEX_CONSTANTS, &bytes, &size));
    assert(bytes == NULL && size == 0 && !rsx_native_draw_payload_seal(s));
}

static void ownership_alignment_and_lifecycle(void) {
    rsx_native_draw_payload_store s;
    allocator_state a; init(&s, &a, 16384, 32); fill_data(41);
    unsigned char saved[4][16];
    for (unsigned k = 0; k < 4; ++k) memcpy(saved[k], data[k], sizeof(saved[k]));
    rsx_native_draw_payload_input in = input(0x100000007ULL);
    rsx_native_draw_payload_record r;
    uint32_t index = UINT32_MAX;
    assert(rsx_native_draw_payload_append(&s, &in, &index) && index == 0);
    assert(rsx_native_draw_payload_get_record(&s, index, &r));
    assert(r.draw_ordinal == 0x100000007ULL && r.vertex_offset == 0 && r.index_offset == 256);
    assert(r.vertex_constants_offset == 512 && r.pixel_constants_offset == 768 && s.bytes_used == 779);
    memset(data, 0xCC, sizeof(data)); memset(&in, 0, sizeof(in));
    check_span(&s, &r, RSX_NATIVE_DRAW_PAYLOAD_VERTEX, saved[0], 3);
    check_span(&s, &r, RSX_NATIVE_DRAW_PAYLOAD_INDEX, saved[1], 5);
    check_span(&s, &r, RSX_NATIVE_DRAW_PAYLOAD_VERTEX_CONSTANTS, saved[2], 7);
    check_span(&s, &r, RSX_NATIVE_DRAW_PAYLOAD_PIXEL_CONSTANTS, saved[3], 11);
    for (size_t at = 0; at < s.bytes_used; ++at)
        if ((at >= 3 && at < 256) || (at >= 261 && at < 512) || (at >= 519 && at < 768))
            assert(s.bytes[at] == 0); /* Allocation is poisoned; published gaps must be initialized. */
    unsigned char prefix[779]; memcpy(prefix, s.bytes, sizeof(prefix));
    in = constants_only(9); in.vertex_constants.size = 5000;
    uint8_t* old = s.bytes;
    assert(rsx_native_draw_payload_append(&s, &in, &index) && index == 1);
    assert(s.bytes != old && !memcmp(s.bytes, prefix, sizeof(prefix)));
    assert(rsx_native_draw_payload_get_record(&s, index, &r));
    assert(r.vertex_constants_offset == 1024 && r.vertex_constants_size == 5000);
    assert(!r.vertex_size && !r.index_size && !r.pixel_constants_size);
    for (size_t at = 779; at < 1024; ++at) assert(s.bytes[at] == 0);
    check_span(&s, &r, RSX_NATIVE_DRAW_PAYLOAD_VERTEX_CONSTANTS, data[2], 5000);
    assert(rsx_native_draw_payload_seal(&s) && s.state == RSX_NATIVE_DRAW_PAYLOAD_SEALED);
    assert(!rsx_native_draw_payload_seal(&s));
    index = 123; assert(!rsx_native_draw_payload_append(&s, &in, &index) && index == 123);
    assert(s.state == RSX_NATIVE_DRAW_PAYLOAD_SEALED && s.record_count == 2);
    size_t capacity = s.bytes_capacity; unsigned calls = a.calls;
    assert(rsx_native_draw_payload_reset(&s) && !s.bytes_used && !rsx_native_draw_payload_count(&s));
    assert(s.bytes_capacity == capacity && a.calls == calls && s.state == RSX_NATIVE_DRAW_PAYLOAD_OPEN);
    assert(!rsx_native_draw_payload_get_record(&s, 0, &r));
    check_span(&s, &r, RSX_NATIVE_DRAW_PAYLOAD_VERTEX_CONSTANTS, NULL, 0);
    fill_data(77); in = constants_only(10);
    assert(rsx_native_draw_payload_append(&s, &in, NULL) && a.calls == calls);
    assert(rsx_native_draw_payload_get_record(&s, 0, &r) && r.vertex_constants_offset == 0 && r.draw_ordinal == 10);
    check_span(&s, &r, RSX_NATIVE_DRAW_PAYLOAD_VERTEX_CONSTANTS, data[2], 1);
    destroy(&s, &a);
    puts("ownership, alignment, deterministic gaps and lifecycle passed");
}

static void optional_spans(void) {
    const size_t lengths[] = {1, 255, 256, 257};
    for (unsigned mask = 0; mask < 8; ++mask) for (unsigned l = 0; l < 4; ++l) {
        rsx_native_draw_payload_store s; allocator_state a; init(&s, &a, 16384, 2);
        fill_data(mask+l); rsx_native_draw_payload_input in = input(mask);
        rsx_native_draw_payload_span* spans[] = {&in.vertex, &in.index, &in.vertex_constants, &in.pixel_constants};
        for (unsigned k = 0; k < 4; ++k) spans[k]->size = lengths[l];
        if (!(mask & 1u)) { in.vertex.data = NULL; in.vertex.size = 0; }
        if (!(mask & 2u)) { in.index.data = NULL; in.index.size = 0; }
        if (!(mask & 4u)) { in.pixel_constants.data = NULL; in.pixel_constants.size = 0; }
        assert(rsx_native_draw_payload_append(&s, &in, NULL));
        rsx_native_draw_payload_record r; assert(rsx_native_draw_payload_get_record(&s, 0, &r));
        assert(r.vertex_offset % 256 == 0 && r.index_offset % 256 == 0);
        assert(r.vertex_constants_offset % 256 == 0 && r.pixel_constants_offset % 256 == 0);
        for (unsigned k = 0; k < 4; ++k) check_span(&s, &r, (rsx_native_draw_payload_kind)k, spans[k]->data, spans[k]->size);
        destroy(&s, &a);
    }
    puts("32 optional-buffer and alignment-boundary combinations passed");
}

static void rejected_append_and_caps(void) {
    for (unsigned invalid = 0; invalid < 11; ++invalid) {
        rsx_native_draw_payload_store s; allocator_state a; init(&s, &a, 8192, 8);
        fill_data(19); rsx_native_draw_payload_input in = input(5);
        assert(rsx_native_draw_payload_append(&s, &in, NULL));
        const size_t used = s.bytes_used; unsigned char before[779]; memcpy(before, s.bytes, used);
        rsx_native_draw_payload_record prior = s.records[0];
        rsx_native_draw_payload_span* spans[] = {&in.vertex, &in.index, &in.vertex_constants, &in.pixel_constants};
        if (invalid < 8) {
            if (invalid & 1u) spans[invalid/2]->size = 0;
            else spans[invalid/2]->data = NULL;
        } else if (invalid == 8) { in.vertex_constants.data = NULL; in.vertex_constants.size = 0; }
        else if (invalid == 9) { in.vertex.data = s.bytes+s.bytes_capacity-1; in.vertex.size = 1; }
        else { in.index.data = s.records; in.index.size = sizeof(*s.records); }
        uint32_t index = 999; unsigned calls = a.calls;
        assert(!rsx_native_draw_payload_append(&s, &in, &index) && index == 999 && a.calls == calls);
        assert_failed(&s, 1, used);
        assert(!memcmp(before, s.bytes, used) && !memcmp(&prior, s.records, sizeof(prior)));
        assert(rsx_native_draw_payload_reset(&s)); destroy(&s, &a);
    }
    for (unsigned exact = 0; exact < 2; ++exact) {
        rsx_native_draw_payload_store s; allocator_state a; init(&s, &a, exact ? 513 : 512, 2);
        rsx_native_draw_payload_input in = input(1);
        in.vertex.size = in.index.size = in.vertex_constants.size = 1;
        in.pixel_constants.data = NULL; in.pixel_constants.size = 0;
        uint32_t index = 999;
        assert(rsx_native_draw_payload_append(&s, &in, &index) == (int)exact);
        if (exact) assert(s.bytes_used == 513 && s.bytes_capacity <= 513 && index == 0);
        else { assert_failed(&s, 0, 0); assert(index == 999 && a.calls == 0); }
        destroy(&s, &a);
    }
    rsx_native_draw_payload_store s; allocator_state a; init(&s, &a, 65536, 17);
    rsx_native_draw_payload_input in = constants_only(1);
    for (unsigned i = 0; i < 17; ++i) {
        in.draw_ordinal = i;
        assert(rsx_native_draw_payload_append(&s, &in, NULL));
        assert(s.records_capacity <= 17 && s.bytes_capacity <= 65536);
    }
    assert(s.records_capacity == 17);
    const size_t used = s.bytes_used; unsigned calls = a.calls;
    uint32_t index = 999; assert(!rsx_native_draw_payload_append(&s, &in, &index));
    assert(index == 999 && a.calls == calls); assert_failed(&s, 17, used);
    destroy(&s, &a);
    puts("invalid spans, storage alias rejection, padding-inclusive byte cap and draw cap passed");
}

static void allocation_failures(void) {
    for (unsigned initial = 0; initial < 2; ++initial) for (unsigned stage = 1; stage <= 2; ++stage) {
        rsx_native_draw_payload_store s; allocator_state a; init(&s, &a, 65536, 32);
        rsx_native_draw_payload_input in = constants_only(1);
        if (!initial) for (unsigned i = 0; i < 16; ++i) {
            in.draw_ordinal = i; assert(rsx_native_draw_payload_append(&s, &in, NULL));
        }
        const uint32_t count = s.record_count; const size_t used = s.bytes_used;
        unsigned char before[4096]; rsx_native_draw_payload_record records[16];
        assert(used <= sizeof(before));
        if (used) memcpy(before, s.bytes, used);
        if (count) memcpy(records, s.records, (size_t)count*sizeof(*records));
        a.fail_call = a.calls+stage; uint32_t index = 999;
        assert(!rsx_native_draw_payload_append(&s, &in, &index) && index == 999);
        assert(a.calls == a.fail_call); assert_failed(&s, count, used);
        if (used) assert(!memcmp(before, s.bytes, used));
        if (count) assert(!memcmp(records, s.records, (size_t)count*sizeof(*records)));
        a.fail_call = 0; assert(rsx_native_draw_payload_reset(&s));
        assert(rsx_native_draw_payload_append(&s, &in, &index) && index == 0);
        assert(rsx_native_draw_payload_seal(&s)); destroy(&s, &a);
    }
    puts("four first/growth allocation failures preserve publication and recover only by reset");
}

static void metadata_alias_and_overflow(void) {
    rsx_native_draw_payload_store s; allocator_state a; init(&s, &a, 16384, 32);
    fill_data(3); rsx_native_draw_payload_input in = constants_only(1);
    assert(rsx_native_draw_payload_append(&s, &in, NULL));
    in.vertex_constants.size = 5000;
    /* Metadata in unused owned storage must be copied before growth moves it. */
    rsx_native_draw_payload_input* alias = (rsx_native_draw_payload_input*)(s.bytes+256);
    memcpy(alias, &in, sizeof(in));
    assert(rsx_native_draw_payload_append(&s, alias, NULL));
    rsx_native_draw_payload_record r; assert(rsx_native_draw_payload_get_record(&s, 1, &r));
    check_span(&s, &r, RSX_NATIVE_DRAW_PAYLOAD_VERTEX_CONSTANTS, data[2], 5000);
    destroy(&s, &a);
    for (unsigned kind = 0; kind < 4; ++kind) {
        init(&s, &a, SIZE_MAX, UINT32_MAX); in = constants_only(1);
        if (kind == 0) { in.vertex.data = (const void*)(UINTPTR_MAX-3); in.vertex.size = 8; }
        else if (kind == 1) in.vertex_constants.size = SIZE_MAX;
        else if (kind == 2) s.bytes_used = SIZE_MAX-10; /* Align overflow without giant allocation. */
        else s.record_count = UINT32_MAX; /* Counter increment must not wrap. */
        size_t used = s.bytes_used; uint32_t count = s.record_count; uint32_t index = 999;
        assert(!rsx_native_draw_payload_append(&s, &in, &index) && index == 999 && a.calls == 0);
        assert_failed(&s, count, used); destroy(&s, &a);
    }
    puts("input metadata relocation and four overflow guards passed without huge allocations");
}

static void accessor_and_null_guards(void) {
    rsx_native_draw_payload_store s; allocator_state a; init(&s, &a, 4096, 2);
    rsx_native_draw_payload_input in = constants_only(7); rsx_native_draw_payload_record r;
    assert(!rsx_native_draw_payload_get_record(&s, 0, &r));
    assert(rsx_native_draw_payload_append(&s, &in, NULL));
    assert(rsx_native_draw_payload_get_record(&s, 0, &r));
    assert(!rsx_native_draw_payload_get_record(&s, 1, &r));
    assert(!rsx_native_draw_payload_get_record(&s, 0, NULL));
    for (unsigned corrupt = 0; corrupt < 3; ++corrupt) {
        rsx_native_draw_payload_record bad = r;
        if (corrupt == 0) bad.vertex_constants_offset = UINT64_MAX;
        else if (corrupt == 1) bad.vertex_constants_size = UINT64_MAX;
        else bad.vertex_constants_size = 2;
        const void* bytes = (const void*)(uintptr_t)1; size_t size = SIZE_MAX;
        assert(!rsx_native_draw_payload_get_bytes(&s, &bad, RSX_NATIVE_DRAW_PAYLOAD_VERTEX_CONSTANTS, &bytes, &size));
        assert(bytes == NULL && size == 0);
    }
    const void* bytes = (const void*)(uintptr_t)1; size_t size = SIZE_MAX;
    assert(!rsx_native_draw_payload_get_bytes(&s, &r, (rsx_native_draw_payload_kind)4, &bytes, &size));
    assert(!bytes && !size);
    assert(rsx_native_draw_payload_seal(&s));
    check_span(&s, &r, RSX_NATIVE_DRAW_PAYLOAD_VERTEX_CONSTANTS, data[2], 1);
    destroy(&s, &a);
    assert(!rsx_native_draw_payload_reset(&s) && !rsx_native_draw_payload_seal(&s));
    assert(!rsx_native_draw_payload_append(&s, &in, NULL));
    assert(!rsx_native_draw_payload_init(NULL, 1, 1));
    assert(!rsx_native_draw_payload_init(&s, 0, 1) && !rsx_native_draw_payload_init(&s, 1, 0));
    rsx_native_draw_payload_allocator invalid = { moving_reallocate, NULL, &a };
    assert(!rsx_native_draw_payload_init_with_allocator(&s, 1, 1, &invalid));
    assert(!rsx_native_draw_payload_append(NULL, &in, NULL));
    assert(!rsx_native_draw_payload_get_record(NULL, 0, &r));
    assert(!rsx_native_draw_payload_reset(NULL) && !rsx_native_draw_payload_seal(NULL));
    assert(rsx_native_draw_payload_count(NULL) == 0);
    assert(rsx_native_draw_payload_get_state(NULL) == RSX_NATIVE_DRAW_PAYLOAD_UNINITIALIZED);
    rsx_native_draw_payload_destroy(NULL);
    assert(rsx_native_draw_payload_init(&s, 4096, 2));
    assert(rsx_native_draw_payload_seal(&s) && rsx_native_draw_payload_reset(&s));
    assert(rsx_native_draw_payload_append(&s, &in, NULL));
    assert(!rsx_native_draw_payload_append(&s, NULL, NULL));
    assert(s.state == RSX_NATIVE_DRAW_PAYLOAD_FAILED);
    rsx_native_draw_payload_destroy(&s);
    puts("accessor range/state/null guards and standard allocator lifecycle passed");
}

int main(void) {
    ownership_alignment_and_lifecycle(); optional_spans(); rejected_append_and_caps();
    allocation_failures(); metadata_alias_and_overflow(); accessor_and_null_guards();
    puts("actual-source native draw payload tests passed");
    return 0;
}
