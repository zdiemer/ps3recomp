#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"
#include "rsx_texture_hash.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE4_1__) || defined(_M_X64)
#include <smmintrin.h>
#define RSX_COMPACT_SIMD 1
#endif

static inline float rsx_compact_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static inline float rsx_compact_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    for (u32 component = 0; component < size && component < 4;
         component++) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            out[component] = rsx_compact_be_f32(source + component * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[component] = rsx_compact_be_f16(source + component * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[component] = source[component] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[component] = (float)source[component];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = value / 32767.0f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = (float)value;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 word =
                ((u32)source[0] << 24) | ((u32)source[1] << 16) |
                ((u32)source[2] << 8) | (u32)source[3];
            s32 x = (s32)(word & 0x7FF);
            s32 y = (s32)((word >> 11) & 0x7FF);
            s32 z = (s32)((word >> 22) & 0x3FF);
            if (x & 0x400) x -= 0x800;
            if (y & 0x400) y -= 0x800;
            if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->mask = input_mask & 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        plan->slot_by_attr[attr] = -1;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->mask & (1u << attr)))
            continue;
        const u32 slot = plan->count++;
        plan->attrs[slot] = (u8)attr;
        plan->slot_by_attr[attr] = (s8)slot;
    }
    plan->stride = (u32)plan->count * 16u;
}

void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !rsx || !layout)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &fetch->desc);
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        fetch->stride =
            fetch->desc.stride ? fetch->desc.stride : fetch->elem_size;
        rsx_dsp_vertex_default(rsx, attr, fetch->default_value);
        if (attr == 3 &&
            fetch->default_value[0] == 0.0f &&
            fetch->default_value[1] == 0.0f &&
            fetch->default_value[2] == 0.0f &&
            fetch->default_value[3] == 1.0f) {
            fetch->default_value[0] = 1.0f;
            fetch->default_value[1] = 1.0f;
            fetch->default_value[2] = 1.0f;
        }
    }
}

u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR])
{
    u32 packed = 0, declared = 0;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_dsp_vertex_attr desc;
        rsx_dsp_get_vertex_attr(rsx, attr, &desc);
        if (offsets)
            offsets[attr] = packed;
        if (!desc.type || !desc.size)
            continue;
        packed += rsx_vertex_attrib_size(desc.type, desc.size);
        if (desc.stride > declared)
            declared = desc.stride;
    }
    return declared ? declared : packed;
}

void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes)
{
    if (!plan)
        return;
    plan->inline_data = NULL;
    plan->inline_bytes = 0;
    if (!data || !bytes || !rsx)
        return;
    plan->inline_stride = rsx_vertex_inline_layout(rsx, plan->inline_off);
    plan->inline_data = data;
    plan->inline_bytes = bytes;
}

void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (!plan)
        return;
    u32 vertex_first=UINT_MAX, vertex_last=0;
    for (u32 i=0; refs && i<count; ++i) {
        const u32 element=(refs[i].vertex_id+refs[i].base_index)&0xFFFFFu;
        if (element<vertex_first) vertex_first=element;
        if (element>vertex_last) vertex_last=element;
    }
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        /* INLINE_ARRAY: the stream is already one contiguous, correctly
         * strided array per attribute, which is exactly what stable_base
         * describes -- so it needs no resolver and no bounds fallback, only
         * the right base. */
        if (plan->inline_data) {
            const u32 stride =
                plan->inline_stride ? plan->inline_stride : fetch->stride;
            const u32 off = plan->inline_off[attr];
            if (!stride || off + fetch->elem_size > stride)
                continue;
            const u32 verts = plan->inline_bytes / stride;
            if (!verts)
                continue;
            fetch->stride = stride;
            fetch->stable_base = plan->inline_data + off;
            fetch->stable_first = 0;
            fetch->stable_last = verts - 1;
            continue;
        }

        if (!plan->guest_ptr)
            continue;

        u32 first = vertex_first;
        u32 last = vertex_last;
        if (fetch->desc.frequency>1) {
        first=UINT_MAX; last=0;
        for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index,
                fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }
        }

        const u64 prefix =
            (u64)plan->base_offset + (u64)fetch->desc.offset;
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        float* value = (float*)(out + slot * 16u);
        if (!fetch->desc.type || !fetch->desc.size) {
            memcpy(value, fetch->default_value, 16);
            continue;
        }

        const u32 element = rsx_vertex_element_index(
            ref->vertex_id, ref->base_index, fetch->desc.frequency,
            (plan->divider_mask >> attr) & 1u);
        const u8* source = NULL;
        if (fetch->stable_base &&
            element >= fetch->stable_first && element <= fetch->stable_last) {
            source = fetch->stable_base +
                (u64)(element - fetch->stable_first) * fetch->stride;
        } else if (plan->inline_data) {
            /* An inline draw has no vertex array to fall back to: the array
             * offsets still hold whatever the last ordinary draw bound, and
             * reading them would feed this draw another mesh's vertices.
             * An attribute the stream does not supply gets its default,
             * which is what the hardware feeds. */
            source = NULL;
        } else if (plan->guest_ptr) {
            const u32 offset =
                plan->base_offset + fetch->desc.offset +
                element * fetch->stride;
            source = plan->guest_ptr(
                plan->guest_user, fetch->desc.location, offset,
                fetch->elem_size);
        }

        if (source && rsx_vertex_decode_element(
                fetch->desc.type, fetch->desc.size, source, value))
            continue;
        if (attr == 0)
            return 0;
        memcpy(value, fetch->default_value, 16);
    }
    return 1;
}

/* Native samples concentrated in SINT16 byte swaps/conversions and the F32
 * helper. Decode their lanes together, with exact-width reads at buffer ends.
 * Other formats continue through the scalar reference decoder. */
static inline int rsx_vertex_decode_batch_element(
    u32 type, u32 size, const u8* source, float value[4])
{
#ifdef RSX_COMPACT_SIMD
    if (type==RSX_VTX_TYPE_SINT16 && size && size<=4) {
        u64 packed=0;
        switch (size) {
        case 4: memcpy(&packed,source,8); break;
        case 3: {
            u32 lo; u16 hi;
            memcpy(&lo,source,4); memcpy(&hi,source+4,2);
            packed=(u64)lo|((u64)hi<<32); break;
        }
        case 2: { u32 lo; memcpy(&lo,source,4); packed=lo; break; }
        case 1: { u16 lo; memcpy(&lo,source,2); packed=lo; break; }
        }
        const __m128i words=_mm_shuffle_epi8(_mm_cvtsi64_si128((long long)packed),
            _mm_setr_epi8(1,0,3,2,5,4,7,6,-1,-1,-1,-1,-1,-1,-1,-1));
        _mm_storeu_ps(value,_mm_cvtepi32_ps(_mm_cvtepi16_epi32(words)));
        if (size<4) value[3]=1.0f;
        return 1;
    }
#endif
    return rsx_vertex_decode_element(type,size,source,value);
}

int rsx_vertex_fetch_many(const rsx_vertex_fetch_plan* plan,
    const rsx_vertex_ref* refs, u32 count, u8* out)
{
    if (!plan || (!refs && count) || (!out && plan->layout.stride && count)) return 0;
    /* Rare/fallback plans retain the exact scalar resolver semantics. Inline
     * streams can have out-of-range references, so leave them on that path. */
    int fast = !plan->inline_data;
    for (u32 slot=0; slot<plan->layout.count; ++slot) {
        const rsx_vertex_fetch_attr* f=&plan->attr[plan->layout.attrs[slot]];
        if (f->desc.type && f->desc.size && (!f->stable_base || f->desc.frequency>1)) fast=0;
    }
    if (!fast) {
        for (u32 i=0; i<count; ++i)
            if (!rsx_vertex_fetch_one(plan,&refs[i],out+(u64)i*plan->layout.stride)) return 0;
        return 1;
    }
    for (u32 slot=0; slot<plan->layout.count; ++slot) {
        const u32 attr=plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* f=&plan->attr[attr];
        const u32 type=f->desc.type, size=f->desc.size;
        for (u32 i=0; i<count; ++i) {
            float* value=(float*)(out+(u64)i*plan->layout.stride+slot*16);
            if (!type || !size) { memcpy(value,f->default_value,16); continue; }
            const u32 element=(refs[i].vertex_id+refs[i].base_index)&0xFFFFFu;
            if (element<f->stable_first || element>f->stable_last) {
                if (!rsx_vertex_fetch_one(plan,&refs[i],out+(u64)i*plan->layout.stride)) return 0;
                continue;
            }
            const u8* source=f->stable_base+(u64)(element-f->stable_first)*f->stride;
            if (type==RSX_VTX_TYPE_FLOAT) {
                value[0]=value[1]=value[2]=0; value[3]=1;
                for (u32 k=0; k<size && k<4; ++k) value[k]=rsx_compact_be_f32(source+k*4);
            } else if (!rsx_vertex_decode_batch_element(type,size,source,value)) {
                if (attr==0) return 0;
                memcpy(value,f->default_value,16);
            }
        }
    }
    return 1;
}

enum { VERTEX_CACHE_SLOTS=2048 };
#define VERTEX_CACHE_BUDGET (128ull*1024*1024)
typedef struct rsx_vertex_payload_entry {
    rsx_vertex_fetch_plan key;
    u8* storage;
    u32 count, source_bytes[RSX_DSP_NUM_VERTEX_ATTR];
    u64 storage_size, hash;
} rsx_vertex_payload_entry;

void rsx_vertex_payload_cache_clear(rsx_vertex_payload_cache* cache)
{
    if (!cache) return;
    rsx_vertex_payload_entry* entries=(rsx_vertex_payload_entry*)cache->entries;
    if (entries) {
        for (u32 i=0; i<VERTEX_CACHE_SLOTS; ++i) free(entries[i].storage);
        free(entries);
    }
    memset(cache,0,sizeof(*cache));
}

int rsx_vertex_fetch_cached(rsx_vertex_payload_cache* cache,
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs,
    u32 count, u8* out)
{
    if (!cache) return rsx_vertex_fetch_many(plan,refs,count,out);
    if (!plan || (!refs && count) || (!out && plan->layout.stride && count)) return 0;
    u32 spans[RSX_DSP_NUM_VERTEX_ATTR]={0};
    const u64 ref_bytes=(u64)count*sizeof(*refs);
    const u64 payload_bytes=(u64)count*plan->layout.stride;
    u64 source_bytes=0;
    rsx_vertex_fetch_plan key={0};
    key.layout=plan->layout;
    if (plan->inline_data || count<16 || !payload_bytes || payload_bytes>1024*1024) goto bypass;
    u32 first=UINT32_MAX, last=0;
    for (u32 i=0; i<count; ++i) {
        const u32 element=(refs[i].vertex_id+refs[i].base_index)&0xFFFFFu;
        if (element<first) first=element;
        if (element>last) last=element;
    }
    for (u32 slot=0; slot<plan->layout.count; ++slot) {
        const u32 attr=plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* f=&plan->attr[attr];
        if (!f->desc.type || !f->desc.size) {
            memcpy(key.attr[attr].default_value,f->default_value,sizeof(f->default_value));
            continue;
        }
        if (f->desc.type>7) goto bypass;
        if (!f->stable_base || f->desc.frequency>1 || first<f->stable_first || last>f->stable_last)
            goto bypass;
        const u64 size=(u64)(f->stable_last-f->stable_first)*f->stride+f->elem_size;
        if (!size || size>1024*1024) goto bypass;
        spans[attr]=(u32)size; source_bytes+=size;
        /* Unused registers, resolver metadata and enabled-attribute defaults do
         * not affect this mapped conversion. Avoid creating variants for them. */
        key.attr[attr].desc.type=f->desc.type; key.attr[attr].desc.size=f->desc.size;
        key.attr[attr].stride=f->stride; key.attr[attr].elem_size=f->elem_size;
        key.attr[attr].stable_base=f->stable_base;
        key.attr[attr].stable_first=f->stable_first; key.attr[attr].stable_last=f->stable_last;
    }
    if (source_bytes>4*1024*1024) goto bypass;
    if (!cache->entries) {
        cache->entries=calloc(VERTEX_CACHE_SLOTS,sizeof(rsx_vertex_payload_entry));
        if (!cache->entries) goto bypass;
    }
    const u64 hash=rsx_texture_hash(&key,sizeof(key)) ^ rsx_texture_hash(refs,(size_t)ref_bytes);
    rsx_vertex_payload_entry* entries=(rsx_vertex_payload_entry*)cache->entries;
    const u32 base=(u32)hash&(VERTEX_CACHE_SLOTS-1);
    rsx_vertex_payload_entry* entry=NULL;
    /* Four probes preserve hot entries through ordinary hash collisions. */
    for (u32 probe=0; probe<4; ++probe) {
        rsx_vertex_payload_entry* candidate=&entries[(base+probe)&(VERTEX_CACHE_SLOTS-1)];
        if (candidate->storage && candidate->hash==hash && candidate->count==count &&
            !memcmp(&candidate->key,&key,sizeof(key)) &&
            !memcmp(candidate->storage,refs,(size_t)ref_bytes)) { entry=candidate; break; }
        if (!entry || !candidate->storage) entry=candidate;
    }
    const u64 storage_size=ref_bytes+source_bytes+payload_bytes;
    if (entry->storage && entry->hash==hash && entry->count==count &&
        !memcmp(&entry->key,&key,sizeof(key)) &&
        !memcmp(entry->storage,refs,(size_t)ref_bytes)) {
        const u8* snapshot=entry->storage+ref_bytes;
        int same=1;
        for (u32 attr=0; attr<RSX_DSP_NUM_VERTEX_ATTR; ++attr) {
            if (spans[attr]) {
                cache->compared_bytes+=spans[attr];
                if (memcmp(snapshot,plan->attr[attr].stable_base,spans[attr])) { same=0; break; }
                snapshot+=spans[attr];
            }
        }
        if (same) {
            memcpy(out,entry->storage+ref_bytes+source_bytes,(size_t)payload_bytes);
            ++cache->hits; return 1;
        }
    }
    ++cache->misses;
    if (cache->bytes-entry->storage_size+storage_size>VERTEX_CACHE_BUDGET) goto bypass;
    u8* storage=(u8*)malloc((size_t)storage_size);
    if (!storage) goto bypass;
    memcpy(storage,refs,(size_t)ref_bytes);
    u8* snapshot=storage+ref_bytes;
    rsx_vertex_fetch_plan frozen=*plan;
    for (u32 attr=0; attr<RSX_DSP_NUM_VERTEX_ATTR; ++attr) if (spans[attr]) {
        memcpy(snapshot,plan->attr[attr].stable_base,spans[attr]);
        frozen.attr[attr].stable_base=snapshot;
        snapshot+=spans[attr];
    }
    /* Convert the same snapshot later used to validate hits. */
    if (!rsx_vertex_fetch_many(&frozen,refs,count,snapshot)) { free(storage); return 0; }
    memcpy(out,snapshot,(size_t)payload_bytes);
    cache->bytes=cache->bytes-entry->storage_size+storage_size;
    free(entry->storage); entry->storage=storage; entry->storage_size=storage_size;
    entry->key=key; entry->count=count; entry->hash=hash;
    memcpy(entry->source_bytes,spans,sizeof(spans));
    return 1;
bypass:
    ++cache->bypasses;
    return rsx_vertex_fetch_many(plan,refs,count,out);
}

static u64 rsx_vertex_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

int rsx_vertex_remap_build_ex(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count, int direct_dense)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;

    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }

    if (count > UINT_MAX / 2u)
        return 0;
    // Dense indexed meshes need no hash/probe chain. Preserve the complete
    // (base_index, vertex_id) identity by requiring a common base first.
    u32 min_id=refs[0].vertex_id, max_id=min_id;
    int common_base=1, ascending=1;
    for (u32 i=1; i<count; ++i) {
        if (refs[i].base_index!=refs[0].base_index) common_base=0;
        if (refs[i].vertex_id<=refs[i-1].vertex_id) ascending=0;
        if (refs[i].vertex_id<min_id) min_id=refs[i].vertex_id;
        if (refs[i].vertex_id>max_id) max_id=refs[i].vertex_id;
    }
    if (common_base && ascending) {
        for (u32 i=0; i<count; ++i) remap->occurrence_to_unique[i]=i;
        return 1;
    }
    const int dense=common_base && (u64)max_id-min_id+1 <= (u64)count*2;
    u32 slot_capacity = 16u;
    const u32 required_slots = count * 2u;
    while (slot_capacity < required_slots) {
        if (slot_capacity > UINT_MAX / 2u)
            return 0;
        slot_capacity *= 2u;
    }
    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0,
               (size_t)remap->slot_capacity * sizeof(*remap->slots));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0,
                   (size_t)remap->slot_capacity * sizeof(*remap->slots));
            remap->generation = 1u;
        }
    }

    u32 unique = 0;
    if (dense && direct_dense) {
        /* A common base and bounded range make vertex_id-min_id an exact
         * identity, so neither a 64-bit key nor a probe chain is needed. */
        for (u32 occurrence = 0; occurrence < count; ++occurrence) {
            const rsx_vertex_ref ref = refs[occurrence];
            rsx_vertex_remap_slot* slot = &remap->slots[ref.vertex_id-min_id];
            if (slot->generation != remap->generation) {
                slot->generation = remap->generation;
                slot->value = unique;
                refs[unique++] = ref;
            }
            remap->occurrence_to_unique[occurrence] = slot->value;
        }
        *unique_count = unique;
        return 1;
    }
    const u32 mask = remap->slot_capacity - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = dense ? ref.vertex_id-min_id : (u32)rsx_vertex_remap_hash(key) & mask;
        for (;;) {
            rsx_vertex_remap_slot* slot = &remap->slots[slot_index];
            if (slot->generation != remap->generation) {
                slot->key = key;
                slot->value = unique;
                slot->generation = remap->generation;
                refs[unique] = ref;
                remap->occurrence_to_unique[occurrence] = unique;
                unique++;
                break;
            }
            if (slot->key == key) {
                remap->occurrence_to_unique[occurrence] = slot->value;
                break;
            }
            slot_index = (slot_index + 1u) & mask;
        }
    }
    *unique_count = unique;
    return 1;
}

int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    return rsx_vertex_remap_build_ex(remap, refs, count, unique_count, 1);
}

u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence)
{
    return remap && occurrence < remap->occurrence_capacity
        ? remap->occurrence_to_unique[occurrence] : occurrence;
}

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap)
{
    if (!remap)
        return;
    free(remap->occurrence_to_unique);
    free(remap->slots);
    memset(remap, 0, sizeof(*remap));
}

int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed)
{
    if (!indexed)
        return 0;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        *indexed = refs_remapped != 0;
        return 1;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_QUADS:
        *indexed = 1;
        return 1;
    default:
        *indexed = 0;
        return 0;
    }
}
